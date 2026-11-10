// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int TTDB_VERSION = 1;
const int TTDB_HEADER_SIZE = 20;
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================


int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
 
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
 
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 || connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    if (!sendFramedFile(sock, STATUS_OK, sourcePath))
        return false;
    shutdown(sock, SHUT_WR);
    return true;

}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    
    TrailerChecker checker;
    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE + 4];
    int status = -1;
    FILE *file = nullptr;
    string message;
 
    while (true)
    {
        ssize_t n = recv(sock, in, sizeof(in), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
 
        size_t len = checker.feed(in, n, out);
        char *body = out;
        if (len > 0 && status == -1)
        {
            status = (unsigned char)out[0];
            body++;
            len--;
            if (status == STATUS_OK)
                file = fopen(outTdbgPath, "wb");
        }
 
        if (status == STATUS_OK && file != nullptr)
            fwrite(body, 1, len, file);
        else if (status == STATUS_ERROR)
            message.append(body, len);
    }
    if (file != nullptr)
        fclose(file);
 
    if (status == -1)
    {
        outError = "no reply received from the load balancer";
        return false;
    }
    if (!checker.verify())
    {
        remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == STATUS_ERROR)
    {
        outError = message;
        return false;
    }
    if (status != STATUS_OK || file == nullptr)
    {
        remove(outTdbgPath);
        outError = "could not save the .tdbg file";
        return false;
    }
    return true;
}


bool requestDebugFile(const char *host, int port, const char *sourcePath, const char *outTdbgPath, string &outError)
{
    FILE *check = fopen(sourcePath, "rb");
    if (check == nullptr)
    {
        outError = string("cannot open source file ") + sourcePath;
        return false;
    }
    fclose(check);
 
    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer";
        return false;
    }
    if (!uploadSource(sock, sourcePath))
    {
        close(sock);
        outError = "upload failed";
        return false;
    }
    bool ok = receiveResponse(sock, outTdbgPath, outError);
    close(sock);
    return ok;
}
 
// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================


bool readAt(int fd, void *buffer, size_t n, int64_t position)
{
    return pread(fd, buffer, n, position) == (ssize_t)n;
}
 
bool readInt(int fd, int64_t &position, int32_t &value)
{
    if (!readAt(fd, &value, 4, position))
        return false;
    position += 4;
    return true;
}

bool readString(int fd, int64_t &position, string &text)
{
    int32_t size;
    if (!readInt(fd, position, size) || size < 0 || size > MAX_STRING_SIZE)
        return false;
    text.resize(size);
    if (size > 0 && !readAt(fd, &text[0], size, position))
        return false;
    position += size;
    return true;
}

bool readVariable(int fd, int64_t &position, Variable &v)
{
    return readString(fd, position, v.name) && readInt(fd, position, v.value);
}
 

bool openTdbg(const char *path, TdbgReader &r)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
 
    int64_t fileSize = lseek(fd, 0, SEEK_END);
    TTDBHeader h;
    bool ok = fileSize >= TTDB_HEADER_SIZE;
    ok = ok && readAt(fd, h.magic, 4, 0);
    ok = ok && readAt(fd, &h.version, 4, 4);
    ok = ok && readAt(fd, &h.stepCount, 4, 8);
    ok = ok && readAt(fd, &h.indexOffset, 8, 12);
    ok = ok && memcmp(h.magic, "TTDB", 4) == 0 && h.version == TTDB_VERSION && h.stepCount > 0;
    ok = ok && h.indexOffset >= TTDB_HEADER_SIZE && h.indexOffset + (int64_t)h.stepCount * 8 == fileSize;
 
    vector<int64_t> index;
    if (ok)
    {
        index.resize(h.stepCount);
        ok = readAt(fd, index.data(), (size_t)h.stepCount * 8, h.indexOffset);
    }
    if (!ok)
    {
        close(fd);
        return false;
    }
 
    r.file_index = fd;
    r.header = h;
    r.index = index;
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    r.index.clear();
    r.index.shrink_to_fit();
    if (r.file_index >= 0)
        close((int)r.file_index);
    r.file_index = -1;
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file_index < 0 || step < 0 || step >= r.header.stepCount)
        return nullptr;
 
    int fd = (int)r.file_index;
    int64_t position = r.index[step];
    int64_t end = (step + 1 < r.header.stepCount) ? r.index[step + 1] : r.header.indexOffset;
 
    Snapshot *s = new Snapshot();
    bool ok = readInt(fd, position, s->stackDepth) && s->stackDepth >= 0 && s->stackDepth <= MAX_STACK_DEPTH;
 
    for (int32_t i = 0; ok && i < s->stackDepth; i++)
    {
        Frame &f = s->callStack[i];
 
        ok = readString(fd, position, f.func_name) && readInt(fd, position, f.argc);
        ok = ok && f.argc >= 0 && f.argc <= MAX_VARS_PER_FRAME;
        for (int32_t j = 0; ok && j < f.argc; j++)
            ok = readVariable(fd, position, f.argv[j]);
 
        ok = ok && readInt(fd, position, f.returnLine) && readInt(fd, position, f.localCount);
        ok = ok && f.localCount >= 0 && f.localCount <= MAX_VARS_PER_FRAME;
        for (int32_t j = 0; ok && j < f.localCount; j++)
            ok = readVariable(fd, position, f.locals[j]);
    }
 
    if (!ok || position != end)
    {
        delete s;
        return nullptr;
    }
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.currentStep = 0;
    s.reader.file_index = -1;  
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;

}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no active session, use: start <source file>" << endl;
        return false;
    }
 
    int64_t last = s.reader.header.stepCount - 1;
    if (delta > 0 && s.currentStep == last)
    {
        cout << "already at the last step" << endl;
        return false;
    }
    if (delta < 0 && s.currentStep == 0)
    {
        cout << "already at the first step" << endl;
        return false;
    }
 
    int64_t target = s.currentStep + delta;
    if (target > last)
        target = last;
    if (target < 0)
        target = 0;
 
    int64_t oldStep = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = oldStep;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    string error;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_TMP, error))
    {
        cout << "Error: " << error << endl;
        return false;
    }
 
    TdbgReader fresh;
    if (!openTdbg(TDBG_TMP, fresh))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        remove(TDBG_TMP);
        return false;
    }
 
    if (s.active_flag)
        closeTdbg(s.reader);
    rename(TDBG_TMP, TDBG_FILE);
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "=== Step " << step << " / " << totalSteps << " ===" << endl;
    cout << "call stack (top first):" << endl;
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &f = s.callStack[i];
        cout << "  #" << i << " " << f.func_name << "(";
        for (int32_t j = 0; j < f.argc; j++)
        {
            if (j > 0)
                cout << ", ";
            cout << f.argv[j].name << "=" << f.argv[j].value;
        }
        cout << ")  locals: ";
        if (f.localCount == 0)
            cout << "none";
        for (int32_t j = 0; j < f.localCount; j++)
        {
            if (j > 0)
                cout << ", ";
            cout << f.locals[j].name << "=" << f.locals[j].value;
        }
        cout << endl;
    }
}
void printHelp()
{
    // Print the commands
    cout << "commands:" << endl;
    cout << "  start <source file>   send the program to the server and begin debugging" << endl;
    cout << "  next                  go one step forward" << endl;
    cout << "  prv                   go one step back" << endl;
    cout << "  jmp next <n>          go n steps forward" << endl;
    cout << "  jmp prv <n>           go n steps back" << endl;
    cout << "  help                  show this text" << endl;
    cout << "  quit | exit           leave the debugger" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================


vector<string> splitWords(const string &line)
{
    vector<string> words;
    string word;
    for (size_t i = 0; i <= line.size(); i++)
    {
        if (i == line.size() || line[i] == ' ' || line[i] == '\t' || line[i] == '\r')
        {
            if (!word.empty())
                words.push_back(word);
            word.clear();
        }
        else
            word += line[i];
    }
    return words;
}
 
bool parsePositive(const string &text, int32_t &number)
{
    if (text.empty() || text.size() > 10)
        return false;
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] < '0' || text[i] > '9')
            return false;
    long long value = atoll(text.c_str());
    if (value < 1 || value > MAX_JUMP)
        return false;
    number = (int32_t)value;
    return true;
}

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    vector<string> words = splitWords(line);
    if (!words.empty() && words[0] == PROGRAM_NAME)
        words.erase(words.begin());
 
    if (!words.empty())
    {
        const string &cmd = words[0];
        int32_t n;
        if (cmd == "start" && words.size() == 2)
            return cmdStart(s, words[1]);
        if (cmd == "next" && words.size() == 1)
            return cmdNext(s);
        if (cmd == "prv" && words.size() == 1)
            return cmdPrv(s);
        if (cmd == "jmp" && words.size() == 3 && words[1] == "next" && parsePositive(words[2], n))
            return cmdJmpNext(s, n);
        if (cmd == "jmp" && words.size() == 3 && words[1] == "prv" && parsePositive(words[2], n))
            return cmdJmpPrv(s, n);
        if (cmd == "help" && words.size() == 1)
        {
            printHelp();
            return true;
        }
        if ((cmd == "quit" || cmd == "exit") && words.size() == 1)
        {
            quitRequested = true;
            return true;
        }
    }
 
    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    if (argc != 3)
    {
        cerr << "usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }
    LB_HOST = argv[1];
    LB_PORT = atoi(argv[2]);
 
    signal(SIGPIPE, SIG_IGN);
 
    DebugSession session;
    initSession(session);
    cout << "Time-Travel Debugger" << endl;
    printHelp();
 
    bool quitRequested = false;
    string line;
    while (!quitRequested)
    {
        cout << PROMPT << flush;
        if (!getline(cin, line))
            break;
        if (splitWords(line).empty())
            continue;
        runCommand(session, line, quitRequested);
    }
 
    closeSession(session);
    return 0;
}
