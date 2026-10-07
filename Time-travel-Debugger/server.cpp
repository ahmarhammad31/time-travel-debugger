// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#ifndef _WIN32
#define _FILE_OFFSET_BITS 64 
#endif

#ifndef _WIN32
#define fseek64(f, off, whence) _fseeki64((f),(__int64)(off),(whence))
#define ftell64(f) ((int64_t)_ftelli64(f))
#else
#define fseek64(f,off,whence) fseeko((f),(off_t)(off),(whence))
#define ftell64(f) ((int64_t)ftello(f))
#endif

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include<unistd.h>
#include<sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever
const int32_t MAX_STEPS = 10000;

//ERROR REPORT
string gError;
void sendError(const string& msg) {
    cout << "Error: " << msg << endl;
}
//helpers
static string lowerStr(string s) {
    for (size_t i = 0;i < s.size();i++) {
        s[i] = (char)tolower((unsigned char)s[i]);
    }
    return s;
}
static string wordAt(const string& line, int32_t index) {
    size_t i = 0, n = line.size();
    int32_t curr = 0;
    while (i < n) {
        while (i < n && isspace((unsigned char)line[i]))i++;
        if (i >= n)break;
        size_t s = i;
		while (i < n && !isspace((unsigned char)line[i]))i++;
        if (curr == index) {
            return line.substr(s, i - s);
        }
        curr++;
    }
    return "";
}
static bool isNumber(const string& s) {
	if (s.empty())return false;
	size_t i = 0;
	if (s[0] == '-' || s[0] == '+')i++;
	for (; i < s.size();i++) {
		if (!isdigit((unsigned char)s[i]))return false;
	}
	return true;
}
static int32_t toInt(const string& s) {
    return (int32_t)strtoll(s.c_str(), nullptr, 10);
}


// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }

    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)return;
        // pushes the value on the stack if max limit is not reached yet.
        Node* n = new Node();
        n->data = val;
        top = n;
        count++;

    }
    T pop()
    {
        // pop the top value on the stack
        if (isEmpty()) return T();
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }
    T& peek()
    {
        // returns the top value on the stack
        if (isEmpty()) throw underflow_error("Stack is empty");
        return top->data;
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* temp = top;
        int32_t ct = 0;
        while (temp != nullptr && ct < maxLen) {
            out[ct] = temp->data;
            ct++;
            temp = temp->next;
        }
        return ct;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
		head = nullptr;
        tail = nullptr;
		stepCount = 0;
    }
    void record(Snapshot* s)
    {
		TimelineNode* n = new TimelineNode();
        n->data = s;
		n->next = nullptr;
		n->prev = tail;
        if (tail != nullptr)tail->next = n;
        else head = n;
        tail = n;
        stepCount++;
        // add record in the timeline
    }
    TimelineNode* begin()
    {
		return head;

    }
    int32_t getStepCount()
    {
		return stepCount;
    }
};

// Core structs
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
    int32_t stackDepth = 0;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
	fwrite(&h.stepCount, sizeof(int32_t), 1, f);
	fwrite(&h.indexOffset, sizeof(int64_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    // reads the next nonblank line
    string line;
    char ch;
    while (true) {
        line.clear();
        bool chk = false;
        while (in.get(ch)) {
            chk = true;
            if (ch == '\n')break;
            line.push_back(ch);
        }
        if (!chk)return false;
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line.erase(0, 3);
        }
        size_t s = 0, e = line.size();
        while (s < e && isspace((unsigned char)line[s])) s++;
        while (e < s && isspace((unsigned char)line[e - 1]))e--;
        line = line.substr(s, e - s);
        if (line.empty())continue;
        if (line.compare(0, 2, "//") == 0)continue;
        out = line;
        return true;
    }
}
string firstWord(const string& line)
{
    // returns first word from the input string
    return wordAt(line, 0);
}
string secondWord(const string& line)
{
    // returns the second word
    return wordAt(line, 1);
}
bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream in(sourcePath, ios::binary);
    if (!in) {
        gError = string("cannot open") + sourcePath;
        return false;
    }
    in.seekg(0, ios::end);
    uint64_t size = (uint64_t)in.tellg();
    in.seekg(0, ios::beg);
    if (size > MAX_SOURCE_BYTES) {
        gError = "source file exceeds the maximum allowed limit";
        return false;
    }

}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position

    int64_t st = (int64_t)ftell(f);
	int32_t size = (int32_t)text.size();
	fwrite(&offsetField, sizeof(int64_t), 1, f);
	fwrite(&size, sizeof(int32_t), 1, f);
	if (size > 0)fwrite(text.data(), 1, (size_t)size, f);
    return st;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
	int64_t offsetField;
	int32_t size;
	if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)return -1;
	if (fread(&size, sizeof(int32_t), 1, f) != 1)return -1;
	outText.resize((size_t)size);
	if (size > 0 && fread(&outText[0], 1, (size_t)size, f) != (size_t)size)return -1;
    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCt = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCt = 0;
	ifstream fin(sourcePath, ios::binary);
    if (!fin) {
		gError = string("cannot open ") + sourcePath;
		return -1;
    }
	FILE* fout = fopen(resolveBinPath, "wb");
    if (!fout) {
		gError = string("cannot open ") + resolveBinPath;
		return -1;
    }
	setvbuf(fout, nullptr, _IOFBF, IO_BUFFER_SIZE);
    string line;
	int64_t pos = 0;
    while (readSourceLine(fin, line)) {
		string s = lowerStr(firstWord(line));
		int64_t offsetField = pos;
        if (s == "func") {
			string name = secondWord(line);
			if (funcCt >= MAX_FUNCS) {
				gError = "too many functions";
				fclose(fout);
				return -1;
			}
            for (int32_t i = 0;i < funcCt;i++) {
                if(funcArray[i].funcName == name) {
                    gError = "function '" + name + "' is already defined";
                    fclose(fout);
                    return -1;
                }
            }
        }
        else if(s=="call"){
			if (patchCt >= MAX_PATCHES) {
				gError = "too many call patches";
				fclose(fout);
				return -1;
			}
			patches[patchCt].byteOffsetOfOffsetField = pos;
			patches[patchCt].targetFuncName = secondWord(line);
            patchCt++;
			offsetField = 0; 
        }
        writeResolveRecord(fout, offsetField, line);
		pos += 8 + 4 + (int64_t)line.size();
    }
    fin.close();
    for (int32_t i = 0;i < patchCt;i++) {
        int64_t tar = -1;
        for (int32_t j = 0;j < funcCt;j++) {
			if (funcArray[j].funcName == patches[i].targetFuncName]) {
				tar = funcArray[j].byteOffsetInResolveBin;
				break;
			}

        }
        if (tar < 0) {
			gError = "call to undefined function '" + patches[i].targetFuncName + "'";
			fclose(fout);
			return -1;
        }
		fseek(fout, (off_t)patches[i].byteOffsetOfOffsetField, SEEK_SET);
		fwrite(&tar, sizeof(int64_t), 1, fout);

    }
    fflush(fout);
	int64_t mainOffset = -1;
    for (int32_t j = 0;j < funcCt;j++) {
		if (funcArray[j].funcName == "main") {
			mainOffset = funcArray[j].byteOffsetInResolveBin;
			break;
		}
    }
    if (ferror(fout)) {
        gError = "failed while wrting resolve.bin";
		fclose(fout);
        return -1;
    }
    fclose(fout);
    if (mainOffset < 0) {
        gError = "no 'main' function defined";
    }
    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}