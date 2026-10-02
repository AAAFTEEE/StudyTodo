// ============================================================
//  studytodo_v4.cpp  作业/学习待办管理器 —— 图形桌面版 (Win32)
//  在 v2 基础上：控制台 -> 图形窗口，并增加大量功能
//    - 科目树 + 任务列表 (ListView) + 进度条/状态栏
//    - 优先级(高/中/低)、截止日期(超期红/临期黄)、标签
//    - 搜索、多视图过滤(全部/未完成/已完成/按科目)、点列头排序
//    - 番茄钟、统计面板、深色/浅色主题、自动保存
//    - 完全兼容读取 v2 生成的 GBK 数据文件 (homework/*.txt)
//  编译(mingw g++)：
//    g++ -std=c++17 -municode -O2 -mwindows studytodo_v4.cpp \
//        -o studytodo_v4.exe -lcomctl32 -lcomdlg32 -lgdi32 -luser32
// ============================================================

#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <commctrl.h>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>
using namespace std;

// ---------------- 全局 ----------------
HINSTANCE g_hInst = nullptr;
HWND g_hMain = nullptr, g_hTree = nullptr, g_hList = nullptr;
HWND g_hStatus = nullptr, g_hProgress = nullptr, g_hStatText = nullptr;
HFONT g_font = nullptr, g_fontBold = nullptr;
const wstring HW_DIR = L"homework";

// ---------------- 数据结构 ----------------
struct Task {
    wstring text;
    bool    done = false;
    int     priority = 0;      // 0=低 1=中 2=高
    wstring due;               // "yyyy-MM-dd" 或空
    wstring tag;
};
struct Subject {
    wstring name;
    vector<Task> tasks;
};
vector<Subject> subjects;
wstring currentName;           // 当前作业单名(不含扩展名)

// 视图状态
enum ViewMode { VM_ALL, VM_UNDONE, VM_DONE, VM_SUBJECT };
ViewMode viewMode = VM_ALL;
int  viewSubject = -1;
wstring searchWord;
enum SortMode { S_ORIG, S_PRIORITY, S_DUE, S_SUBJECT };
SortMode sortMode = S_ORIG;
bool darkTheme = false;

// 可见任务映射：ListView 行 -> (科目下标, 任务下标)
vector<pair<int,int>> g_visible;

// 番茄钟
bool g_pomoRunning = false, g_pomoPaused = false;
int  g_pomoRemain = 25 * 60;
HWND g_hPomoWnd = nullptr, g_hPomoLabel = nullptr, g_hPomoBar = nullptr;
UINT_PTR g_pomoTimer = 0;

// 表格内 in-place 编辑状态
static HWND    g_ipEdit = nullptr;
static WNDPROC g_ipEditOrig = nullptr;
static int     g_ipRow = -1, g_ipCol = -1;
static bool    g_ipCancel = false;

// ---------------- 控件/菜单 ID ----------------
enum {
    ID_TREE = 101, ID_LIST = 102, ID_PROGRESS = 103, ID_STATTEXT = 104,
    IDC_TASK_TEXT = 201, IDC_TASK_PRIO = 202, IDC_TASK_DUE = 203, IDC_TASK_TAG = 204,
    IDC_SUBJ_NAME = 211,
    IDC_OPEN_LIST = 221, IDC_OPEN_OPEN = 222, IDC_OPEN_DEL = 223, IDC_OPEN_REFRESH = 224,
    IDC_POMO_LABEL = 231, IDC_POMO_BAR = 232, IDC_POMO_START = 233,
    IDC_POMO_PAUSE = 234, IDC_POMO_RESET = 235, IDC_POMO_CLOSE = 236,
    IDC_SEARCH_EDIT = 251,
    IDM_NEW = 301, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_OPENFOLDER, IDM_EXIT,
    IDM_ADDSUBJ, IDM_RENSUBJ, IDM_DELSUBJ,
    IDM_ADDTASK, IDM_EDITTASK, IDM_DELTASK, IDM_TOGGLE, IDM_INSBEFORE, IDM_INSAFTER,
    IDM_VIEWALL, IDM_VIEWUNDONE, IDM_VIEWDONE, IDM_VIEWSUBJ,
    IDM_SEARCH, IDM_CLEARSEARCH,
    IDM_SORTORIG, IDM_SORTPRI, IDM_SORTDUE, IDM_SORTSUBJ,
    IDM_THEME, IDM_POMO, IDM_STATS, IDM_ABOUT, IDM_HELP
};

// ---------------- 编码工具 (GBK <-> UTF-16) ----------------
static wstring gbkToWide(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static string wideToGBK(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring trimW(const wstring& s) {
    size_t l = s.find_first_not_of(L" \t\r\n");
    if (l == wstring::npos) return L"";
    size_t r = s.find_last_not_of(L" \t\r\n");
    return s.substr(l, r - l + 1);
}

// ---------------- 文件操作 ----------------
static wstring safeFileName(wstring name) {
    for (wchar_t& c : name)
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
    return name;
}

static vector<wstring> listHomeworkFiles() {
    vector<wstring> files;
    wstring pattern = HW_DIR + L"\\*.txt";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    sort(files.begin(), files.end());
    return files;
}

static bool writeHomeworkFile(const wstring& name) {
    CreateDirectoryW(HW_DIR.c_str(), nullptr);
    wstring path = HW_DIR + L"\\" + name + L".txt";
    wostringstream os;
    for (auto& s : subjects) {
        os << L"# " << s.name << L"\n";
        for (auto& t : s.tasks) {
            os << (t.done ? L"+ " : L"- ") << t.text;
            bool hasAttr = (t.priority != 0) || !t.due.empty() || !t.tag.empty();
            if (hasAttr) {
                if (t.priority != 0) os << L"\t@@PRI=" << t.priority << L"@@";
                if (!t.due.empty())   os << L"\t@@DUE=" << t.due << L"@@";
                if (!t.tag.empty())   os << L"\t@@TAG=" << t.tag << L"@@";
            }
            os << L"\n";
        }
    }
    string gbk = wideToGBK(os.str());
    ofstream fout(path.c_str(), ios::binary);
    if (!fout) return false;
    fout.write(gbk.data(), (streamsize)gbk.size());
    fout.close();
    return true;
}

static void parseTaskAttrs(Task& t, wstring rest) {
    rest = trimW(rest);
    size_t pos = 0;
    while ((pos = rest.find(L"@@", pos)) != wstring::npos) {
        size_t eq = rest.find(L'=', pos + 2);
        size_t end = rest.find(L"@@", eq == wstring::npos ? pos + 2 : eq + 1);
        if (eq == wstring::npos || end == wstring::npos) break;
        wstring key = rest.substr(pos + 2, eq - pos - 2);
        wstring val = rest.substr(eq + 1, end - eq - 1);
        if (key == L"PRI") t.priority = _wtoi(val.c_str());
        else if (key == L"DUE") t.due = trimW(val);
        else if (key == L"TAG") t.tag = trimW(val);
        pos = end + 2;
    }
}

static bool readHomeworkFile(const wstring& filename) {
    wstring path = HW_DIR + L"\\" + filename;
    ifstream fin(path.c_str(), ios::binary);
    if (!fin) return false;
    string all((istreambuf_iterator<char>(fin)), istreambuf_iterator<char>());
    fin.close();
    wstring content = gbkToWide(all);
    wstringstream ss(content);
    wstring line;
    subjects.clear();
    Subject* cur = nullptr;
    while (getline(ss, line)) {
        line = trimW(line);
        if (line.empty()) continue;
        if (line[0] == L'#') {
            subjects.push_back(Subject{trimW(line.substr(1)), {}});
            cur = &subjects.back();
        } else if (line[0] == L'-' || line[0] == L'+') {
            if (!cur) { subjects.push_back(Subject{L"未分类", {}}); cur = &subjects.back(); }
            wstring body = line.substr(1);
            wstring attrs;
            size_t tab = body.find(L'\t');
            if (tab != wstring::npos) { attrs = body.substr(tab + 1); body = body.substr(0, tab); }
            body = trimW(body);
            if (body.empty()) continue;
            Task t;
            t.text = body;
            t.done = (line[0] == L'+');
            parseTaskAttrs(t, attrs);
            cur->tasks.push_back(t);
        }
    }
    return !subjects.empty();
}

// ---------------- 统计 ----------------
static int totalTasks() { int n = 0; for (auto& s : subjects) n += (int)s.tasks.size(); return n; }
static int totalDone()  { int n = 0; for (auto& s : subjects) for (auto& t : s.tasks) if (t.done) ++n; return n; }

// ---------------- 颜色 ----------------
static COLORREF bgColor()        { return darkTheme ? RGB(30,30,34)   : RGB(255,255,255); }
static COLORREF textColor()      { return darkTheme ? RGB(230,230,230) : RGB(20,20,20); }
static COLORREF doneColor()      { return darkTheme ? RGB(110,190,110) : RGB(60,130,60); }

// 学科配色：给每个科目分配一个淡色背景，一眼区分学科
static COLORREF subjBgColor(int idx) {
    static const COLORREF light[] = {
        RGB(219,234,255), // 淡蓝
        RGB(221,246,226), // 淡绿
        RGB(255,238,214), // 淡橙
        RGB(239,226,255), // 淡紫
        RGB(255,226,234), // 淡粉
        RGB(214,248,248), // 淡青
        RGB(255,247,204), // 淡黄
        RGB(255,218,218), // 淡红
    };
    static const COLORREF dark[] = {
        RGB(40,52,78), RGB(38,66,46), RGB(66,56,36), RGB(56,45,72),
        RGB(74,46,58), RGB(36,64,64), RGB(66,62,34), RGB(74,44,44),
    };
    return darkTheme ? dark[idx % 8] : light[idx % 8];
}

// ---------------- 控件辅助 ----------------
static HWND AddButton(HWND parent, int id, const wstring& text, int x, int y, int w, int h) {
    HWND b = CreateWindowW(L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}
static HWND AddEdit(HWND parent, int id, const wstring& text, int x, int y, int w, int h) {
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(e, WM_SETFONT, (WPARAM)g_font, TRUE);
    return e;
}
static HWND AddLabel(HWND parent, const wstring& text, int x, int y, int w, int h) {
    HWND lb = CreateWindowW(L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE,
                            x, y, w, h, parent, nullptr, g_hInst, nullptr);
    SendMessageW(lb, WM_SETFONT, (WPARAM)g_font, TRUE);
    return lb;
}
static HWND AddCombo(HWND parent, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                           CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

// ---------------- 刷新控件 ----------------
static void rebuildTree() {
    TreeView_DeleteAllItems(g_hTree);
    auto addChild = [&](HTREEITEM parent, const wstring& txt, LPARAM lParam) -> HTREEITEM {
        TVINSERTSTRUCTW c{};
        c.hParent = parent;
        c.hInsertAfter = TVI_LAST;
        c.item.mask = TVIF_TEXT | TVIF_PARAM;
        c.item.pszText = (LPWSTR)txt.c_str();
        c.item.lParam = lParam;
        return TreeView_InsertItem(g_hTree, &c);
    };
    HTREEITEM root = addChild(nullptr, currentName.empty() ? L"当前作业单" : currentName, -1);
    addChild(root, L"全部任务", VM_ALL);
    addChild(root, L"未完成", VM_UNDONE);
    addChild(root, L"已完成", VM_DONE);
    for (int i = 0; i < (int)subjects.size(); ++i) {
        wstring label = subjects[i].name + L" (" + to_wstring((int)subjects[i].tasks.size()) + L")";
        addChild(root, label, (LPARAM)(VM_SUBJECT * 10000 + i));
    }
    TreeView_Expand(g_hTree, root, TVE_EXPAND);
}

static void computeVisible() {
    g_visible.clear();
    for (int i = 0; i < (int)subjects.size(); ++i)
        for (int j = 0; j < (int)subjects[i].tasks.size(); ++j) {
            const Task& t = subjects[i].tasks[j];
            if (viewMode == VM_UNDONE && t.done) continue;
            if (viewMode == VM_DONE && !t.done) continue;
            if (viewMode == VM_SUBJECT && viewSubject != i) continue;
            if (!searchWord.empty() && t.text.find(searchWord) == wstring::npos &&
                t.tag.find(searchWord) == wstring::npos) continue;
            g_visible.push_back({i, j});
        }
    if (sortMode == S_PRIORITY) {
        stable_sort(g_visible.begin(), g_visible.end(), [](const pair<int,int>&a, const pair<int,int>&b){
            const Task& x = subjects[a.first].tasks[a.second];
            const Task& y = subjects[b.first].tasks[b.second];
            if (x.priority != y.priority) return x.priority > y.priority;
            return a.second < b.second;
        });
    } else if (sortMode == S_DUE) {
        stable_sort(g_visible.begin(), g_visible.end(), [](const pair<int,int>&a, const pair<int,int>&b){
            const Task& x = subjects[a.first].tasks[a.second];
            const Task& y = subjects[b.first].tasks[b.second];
            wstring dx = x.due.empty() ? L"9999" : x.due;
            wstring dy = y.due.empty() ? L"9999" : y.due;
            if (dx != dy) return dx < dy;
            return a.second < b.second;
        });
    } else if (sortMode == S_SUBJECT) {
        stable_sort(g_visible.begin(), g_visible.end(), [](const pair<int,int>&a, const pair<int,int>&b){
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
    }
}

static void rebuildList() {
    // 记住当前选中项（学科,任务），重建后尽量恢复到原行，避免跳到第一行
    int curSi = -1, curTi = -1;
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < (int)g_visible.size()) {
        curSi = g_visible[sel].first;
        curTi = g_visible[sel].second;
    }
    computeVisible();
    ListView_DeleteAllItems(g_hList);
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    for (int r = 0; r < (int)g_visible.size(); ++r) {
        int si = g_visible[r].first;
        const Task& t = subjects[si].tasks[g_visible[r].second];
        it.iItem = r; it.iSubItem = 0;
        it.pszText = (LPWSTR)(t.done ? L"✔ 完成" : L"☐ 待办");
        ListView_InsertItem(g_hList, &it);
        ListView_SetItemText(g_hList, r, 1, (LPWSTR)subjects[si].name.c_str());
        ListView_SetItemText(g_hList, r, 2, (LPWSTR)t.text.c_str());
    }
    // 恢复选中：优先原任务所在行；不在列表中（被过滤）才退到第一行
    int target = -1;
    if (curSi >= 0) {
        for (int r = 0; r < (int)g_visible.size(); ++r)
            if (g_visible[r].first == curSi && g_visible[r].second == curTi) { target = r; break; }
    }
    if (target < 0 && !g_visible.empty()) target = 0;
    if (target >= 0)
        ListView_SetItemState(g_hList, target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static void updateStatus() {
    int tot = totalTasks(), don = totalDone();
    wstring s = L"作业单：" + (currentName.empty() ? L"未命名" : currentName) +
                L"    总任务 " + to_wstring(tot) + L"    已完成 " + to_wstring(don) +
                L"    完成率 " + to_wstring(tot ? don*100/tot : 0) + L"%";
    SetWindowTextW(g_hStatus, s.c_str());
    SendMessageW(g_hProgress, PBM_SETPOS, tot ? (WPARAM)(don*100/tot) : 0, 0);
    SetWindowTextW(g_hStatText,
        (L"进度 " + to_wstring(tot ? don*100/tot : 0) + L"%  ·  待办 " +
         to_wstring(tot - don) + L" 项").c_str());
}

static void autoSave() { if (!currentName.empty()) writeHomeworkFile(currentName); }

static void resetData() {
    subjects.clear(); currentName.clear();
    viewMode = VM_ALL; viewSubject = -1; searchWord.clear();
    rebuildTree(); rebuildList(); updateStatus();
}

static bool getSelected(int& si, int& ti) {
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_visible.size()) return false;
    si = g_visible[sel].first; ti = g_visible[sel].second;
    return true;
}
static int pickSubjectIdx() { int si, ti; return getSelected(si, ti) ? si : 0; }

// ---------------- 模态循环 ----------------
static void ModalLoop(HWND dlg) {
    EnableWindow(g_hMain, FALSE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(dlg)) break;
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
}

// ==================== 对话框：新增/编辑任务 ====================
static int g_editSubj = -1, g_editIdx = -1;
static bool g_insertMode = false;   // 插入模式（否则追加到末尾）
static int  g_insertPos = 0;

static LRESULT CALLBACK TaskDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        Task* t = (Task*)((CREATESTRUCTW*)l)->lpCreateParams;
        AddLabel(h, L"任务内容：", 14, 16, 90, 20);
        HWND et = AddEdit(h, IDC_TASK_TEXT, t->text, 14, 40, 340, 26);
        AddLabel(h, L"优先级：", 14, 74, 90, 20);
        HWND cb = AddCombo(h, IDC_TASK_PRIO, 14, 98, 150, 200);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"低");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"中");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"高");
        SendMessageW(cb, CB_SETCURSEL, t->priority, 0);
        AddLabel(h, L"截止日期 (yyyy-MM-dd)：", 200, 74, 170, 20);
        AddEdit(h, IDC_TASK_DUE, t->due, 200, 98, 150, 26);
        AddLabel(h, L"标签（逗号分隔）：", 14, 132, 160, 20);
        AddEdit(h, IDC_TASK_TAG, t->tag, 14, 156, 340, 26);
        AddButton(h, IDOK, L"确定", 130, 200, 90, 30);
        AddButton(h, IDCANCEL, L"取消", 240, 200, 90, 30);
        SetFocus(et);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[1024];
            GetDlgItemTextW(h, IDC_TASK_TEXT, buf, 1024);
            wstring text = trimW(buf);
            if (text.empty()) { MessageBoxW(h, L"任务内容不能为空。", L"提示", MB_OK); return 0; }
            int pri = (int)SendMessageW(GetDlgItem(h, IDC_TASK_PRIO), CB_GETCURSEL, 0, 0);
            GetDlgItemTextW(h, IDC_TASK_DUE, buf, 1024);
            wstring due = trimW(buf);
            if (!due.empty() && due.size() != 10) { MessageBoxW(h, L"日期格式应为 yyyy-MM-dd。", L"提示", MB_OK); return 0; }
            GetDlgItemTextW(h, IDC_TASK_TAG, buf, 1024);
            wstring tag = trimW(buf);
            if (g_editIdx >= 0) {
                Task& t = subjects[g_editSubj].tasks[g_editIdx];
                t.text = text; t.priority = pri; t.due = due; t.tag = tag;
            } else {
                Task t; t.text = text; t.priority = pri; t.due = due; t.tag = tag;
                int si = (g_editSubj >= 0 && g_editSubj < (int)subjects.size()) ? g_editSubj : 0;
                if (g_insertMode) {
                    int pos = g_insertPos;
                    if (pos < 0) pos = 0;
                    if (pos > (int)subjects[si].tasks.size()) pos = (int)subjects[si].tasks.size();
                    subjects[si].tasks.insert(subjects[si].tasks.begin() + pos, t);
                } else {
                    subjects[si].tasks.push_back(t);
                }
            }
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void doAddTask() {
    if (subjects.empty()) { MessageBoxW(g_hMain, L"请先新增科目，再添加任务。", L"提示", MB_OK); return; }
    Task tmp;
    g_editSubj = pickSubjectIdx(); g_editIdx = -1;
    HWND d = CreateWindowW(L"TTaskDlg", L"新增任务", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 380, 260, g_hMain, nullptr, g_hInst, &tmp);
    RECT r{}; GetWindowRect(g_hMain, &r);
    int w = 380, h = 260;
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-w)/2, r.top + (r.bottom-r.top-h)/2, w, h, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildList(); updateStatus(); autoSave();
}
static void doEditTask() {
    int si, ti;
    if (!getSelected(si, ti)) { MessageBoxW(g_hMain, L"请先在列表中选择一个任务。", L"提示", MB_OK); return; }
    Task t = subjects[si].tasks[ti];
    g_editSubj = si; g_editIdx = ti;
    HWND d = CreateWindowW(L"TTaskDlg", L"编辑任务", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 380, 260, g_hMain, nullptr, g_hInst, &t);
    RECT r{}; GetWindowRect(g_hMain, &r);
    int w = 380, h = 260;
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-w)/2, r.top + (r.bottom-r.top-h)/2, w, h, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildList(); updateStatus(); autoSave();
}

// ==================== 对话框：新增/编辑科目 ====================
static int g_editSubjIdx = -1;

static LRESULT CALLBACK SubjectDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        wstring* name = (wstring*)((CREATESTRUCTW*)l)->lpCreateParams;
        AddLabel(h, L"科目名称：", 14, 20, 100, 20);
        HWND e = AddEdit(h, IDC_SUBJ_NAME, *name, 14, 46, 250, 26);
        AddButton(h, IDOK, L"确定", 70, 96, 80, 30);
        AddButton(h, IDCANCEL, L"取消", 170, 96, 80, 30);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[256];
            GetDlgItemTextW(h, IDC_SUBJ_NAME, buf, 256);
            wstring name = trimW(buf);
            if (name.empty()) { MessageBoxW(h, L"科目名称不能为空。", L"提示", MB_OK); return 0; }
            for (int i = 0; i < (int)subjects.size(); ++i)
                if (i != g_editSubjIdx && subjects[i].name == name) {
                    MessageBoxW(h, L"该科目名称已存在。", L"提示", MB_OK); return 0;
                }
            if (g_editSubjIdx >= 0) subjects[g_editSubjIdx].name = name;
            else subjects.push_back(Subject{name, {}});
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void doAddSubject() {
    wstring name;
    g_editSubjIdx = -1;
    HWND d = CreateWindowW(L"TSubjectDlg", L"新增科目", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 300, 150, g_hMain, nullptr, g_hInst, &name);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + 80, r.top + 80, 300, 150, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildTree(); updateStatus();
}
static void doRenameSubject() {
    int si, ti;
    if (!getSelected(si, ti)) {
        MessageBoxW(g_hMain, L"请先选中一个任务（选中其所属科目）。\n或在科目树中点击该科目后重命名。", L"提示", MB_OK);
        return;
    }
    wstring name = subjects[si].name;
    g_editSubjIdx = si;
    HWND d = CreateWindowW(L"TSubjectDlg", L"重命名科目", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 300, 150, g_hMain, nullptr, g_hInst, &name);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + 80, r.top + 80, 300, 150, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildTree(); rebuildList(); updateStatus(); autoSave();
}
static void doDeleteSubject() {
    int si, ti;
    if (!getSelected(si, ti)) { MessageBoxW(g_hMain, L"请先选中该科目的任意任务。", L"提示", MB_OK); return; }
    wstring q = L"确定删除科目【" + subjects[si].name + L"】及其全部任务？";
    if (MessageBoxW(g_hMain, q.c_str(), L"删除科目", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    subjects.erase(subjects.begin() + si);
    viewMode = VM_ALL; viewSubject = -1;
    rebuildTree(); rebuildList(); updateStatus(); autoSave();
}

// ==================== 对话框：打开/管理作业单 ====================
static LRESULT CALLBACK OpenDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        AddLabel(h, L"homework 文件夹中已保存的作业单：", 14, 14, 300, 20);
        HWND lb = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER,
                                  14, 40, 340, 220, h, (HMENU)(INT_PTR)IDC_OPEN_LIST, g_hInst, nullptr);
        SendMessageW(lb, WM_SETFONT, (WPARAM)g_font, TRUE);
        AddButton(h, IDC_OPEN_OPEN, L"打开", 28, 276, 90, 30);
        AddButton(h, IDC_OPEN_DEL, L"删除", 134, 276, 90, 30);
        AddButton(h, IDC_OPEN_REFRESH, L"刷新", 240, 276, 60, 30);
        AddButton(h, IDCANCEL, L"取消", 316, 276, 60, 30);
        // 填充列表
        auto files = listHomeworkFiles();
        for (auto& f : files) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)f.c_str());
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_OPEN_REFRESH) {
            HWND lb = GetDlgItem(h, IDC_OPEN_LIST);
            SendMessageW(lb, LB_RESETCONTENT, 0, 0);
            auto files = listHomeworkFiles();
            for (auto& f : files) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)f.c_str());
            return 0;
        }
        if (id == IDC_OPEN_OPEN) {
            HWND lb = GetDlgItem(h, IDC_OPEN_LIST);
            int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) { MessageBoxW(h, L"请先选择一个作业单。", L"提示", MB_OK); return 0; }
            wchar_t buf[256];
            SendMessageW(lb, LB_GETTEXT, sel, (LPARAM)buf);
            // 保存当前数据前先询问
            if (!subjects.empty()) {
                if (MessageBoxW(h, L"当前未保存的修改将被丢弃，继续打开？", L"打开", MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
            }
            if (readHomeworkFile(buf)) {
                wstring fn = buf;
                if (fn.size() > 4 && fn.substr(fn.size()-4) == L".txt") fn.erase(fn.size()-4);
                currentName = fn;
                viewMode = VM_ALL; viewSubject = -1; searchWord.clear();
                DestroyWindow(h);
            } else {
                MessageBoxW(h, L"读取失败或文件为空。", L"错误", MB_OK);
            }
            return 0;
        }
        if (id == IDC_OPEN_DEL) {
            HWND lb = GetDlgItem(h, IDC_OPEN_LIST);
            int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) { MessageBoxW(h, L"请先选择要删除的作业单。", L"提示", MB_OK); return 0; }
            wchar_t buf[256];
            SendMessageW(lb, LB_GETTEXT, sel, (LPARAM)buf);
            wstring q = L"确定删除文件 " + (wstring)buf + L" ？";
            if (MessageBoxW(h, q.c_str(), L"删除", MB_YESNO | MB_ICONWARNING) == IDYES) {
                DeleteFileW((HW_DIR + L"\\" + buf).c_str());
                SendMessageW(lb, LB_RESETCONTENT, 0, 0);
                auto files = listHomeworkFiles();
                for (auto& f : files) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)f.c_str());
            }
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void doOpen() {
    HWND d = CreateWindowW(L"TOpenDlg", L"打开作业单", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 380, 320, g_hMain, nullptr, g_hInst, nullptr);
    RECT r{}; GetWindowRect(g_hMain, &r);
    int w = 380, h = 320;
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-w)/2, r.top + (r.bottom-r.top-h)/2, w, h, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildTree(); rebuildList(); updateStatus();
}

// ==================== 对话框：番茄钟 ====================
static void pomoUpdate() {
    int mm = g_pomoRemain / 60, ss = g_pomoRemain % 60;
    wchar_t buf[64];
    wsprintfW(buf, L"%02d : %02d", mm, ss);
    SetWindowTextW(g_hPomoLabel, buf);
    SendMessageW(g_hPomoBar, PBM_SETPOS, (WPARAM)(g_pomoRemain * 100 / (25*60)), 0);
}
static LRESULT CALLBACK PomoDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hPomoWnd = h;
        g_hPomoLabel = CreateWindowExW(0, L"STATIC", L"25 : 00",
                WS_CHILD | WS_VISIBLE | SS_CENTER, 40, 30, 220, 60, h, nullptr, g_hInst, nullptr);
        g_hPomoBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE, 40, 100, 220, 22, h, (HMENU)(INT_PTR)IDC_POMO_BAR, g_hInst, nullptr);
        SendMessageW(g_hPomoLabel, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        SendMessageW(g_hPomoBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(g_hPomoBar, PBM_SETPOS, 100, 0);
        AddButton(h, IDC_POMO_START, L"开始", 40, 140, 66, 30);
        AddButton(h, IDC_POMO_PAUSE, L"暂停", 116, 140, 66, 30);
        AddButton(h, IDC_POMO_RESET, L"重置", 192, 140, 66, 30);
        AddButton(h, IDC_POMO_CLOSE, L"关闭", 196, 180, 64, 28);
        g_pomoTimer = SetTimer(h, 1, 1000, nullptr);
        return 0;
    case WM_TIMER:
        if (g_pomoRunning && !g_pomoPaused) {
            if (g_pomoRemain > 0) {
                --g_pomoRemain;
                pomoUpdate();
                if (g_pomoRemain == 0) {
                    g_pomoRunning = false;
                    MessageBeep(MB_ICONASTERISK);
                    MessageBoxW(h, L"专注时间结束！休息一下吧。", L"番茄钟", MB_OK | MB_ICONINFORMATION);
                }
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_POMO_START:
            if (!g_pomoRunning) { if (g_pomoRemain <= 0) g_pomoRemain = 25*60; g_pomoRunning = true; g_pomoPaused = false; }
            pomoUpdate(); return 0;
        case IDC_POMO_PAUSE:
            g_pomoPaused = !g_pomoPaused; return 0;
        case IDC_POMO_RESET:
            g_pomoRemain = 25*60; g_pomoRunning = false; g_pomoPaused = false; pomoUpdate(); return 0;
        case IDC_POMO_CLOSE:
            DestroyWindow(h); return 0;
        }
        break;
    case WM_DESTROY:
        if (g_pomoTimer) KillTimer(h, g_pomoTimer);
        g_pomoTimer = 0; g_pomoRunning = false; g_pomoPaused = false;
        g_hPomoWnd = nullptr; g_hPomoLabel = nullptr; g_hPomoBar = nullptr;
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void doPomo() {
    if (g_hPomoWnd && IsWindow(g_hPomoWnd)) { SetForegroundWindow(g_hPomoWnd); return; }
    HWND d = CreateWindowW(L"TPomoDlg", L"番茄钟", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 300, 230, g_hMain, nullptr, g_hInst, nullptr);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-300)/2, r.top + (r.bottom-r.top-230)/2, 300, 230, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
}

// ==================== 对话框：统计 ====================
static LRESULT CALLBACK StatsDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        int y = 14, W = 340;
        int tot = totalTasks(), don = totalDone();
        wchar_t buf[256];
        wsprintfW(buf, L"总体完成率：%d / %d  (%d%%)", don, tot, tot ? don*100/tot : 0);
        AddLabel(h, buf, 14, y, W, 22); y += 30;
        for (int i = 0; i < (int)subjects.size(); ++i) {
            int d = 0; for (auto& t : subjects[i].tasks) if (t.done) ++d;
            int c = (int)subjects[i].tasks.size();
            wstring txt = subjects[i].name + L"  " + to_wstring(d) + L"/" + to_wstring(c) +
                          L"  (" + to_wstring(c ? d*100/c : 0) + L"%)";
            AddLabel(h, txt, 14, y, W, 20); y += 22;
            HWND pb = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
                                      14, y, W, 18, h, nullptr, g_hInst, nullptr);
            SendMessageW(pb, WM_SETFONT, (WPARAM)g_font, TRUE);
            SendMessageW(pb, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessageW(pb, PBM_SETPOS, c ? d*100/c : 0, 0);
            y += 26;
        }
        AddButton(h, IDOK, L"关闭", 140, y + 6, 90, 30);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void doStats() {
    int height = 60 + (int)subjects.size() * 48 + 40;
    HWND d = CreateWindowW(L"TStatsDlg", L"进度统计", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 380, height, g_hMain, nullptr, g_hInst, nullptr);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-380)/2, r.top + (r.bottom-r.top-height)/2, 380, height, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
}

// ==================== 表格内 in-place 编辑 ====================
static void ipCommit();   // 前置声明

static LRESULT CALLBACK IPEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { g_ipCancel = true; ipCommit(); return 0; }
        break;
    case WM_CHAR:
        if (w == VK_RETURN) { ipCommit(); return 0; }
        break;
    case WM_KILLFOCUS:
        if (g_ipEdit == h) ipCommit();
        return 0;
    }
    return CallWindowProcW(g_ipEditOrig, h, m, w, l);
}

static void ipCommit() {
    HWND h = g_ipEdit;
    if (!h) return;
    g_ipEdit = nullptr;              // 防 WM_KILLFOCUS 递归
    int row = g_ipRow, col = g_ipCol;
    bool cancel = g_ipCancel; g_ipCancel = false;
    wchar_t buf[1024]; GetWindowTextW(h, buf, 1024);
    wstring text = trimW(buf);
    bool isCombo = (col == 1);
    int combo = -1;
    if (isCombo) combo = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    DestroyWindow(h);
    if (cancel || row < 0 || row >= (int)g_visible.size()) return;
    int si = g_visible[row].first, ti = g_visible[row].second;
    switch (col) {
    case 1: // 学科：把任务移到目标科目
        if (combo >= 0 && combo != si) {
            Task t = subjects[si].tasks[ti];
            subjects[si].tasks.erase(subjects[si].tasks.begin() + ti);
            subjects[combo].tasks.push_back(t);
        }
        break;
    case 2: // 任务文本
        if (!text.empty()) subjects[si].tasks[ti].text = text;
        break;
    }
    rebuildList(); updateStatus(); autoSave();
}

static void ipStartEdit(int row, int col) {
    if (g_ipEdit) ipCommit();
    if (row < 0 || row >= (int)g_visible.size()) return;
    if (col < 1 || col > 2) col = 2;   // 列定位失败时默认编辑任务列
    int si = g_visible[row].first, ti = g_visible[row].second;
    Task& t = subjects[si].tasks[ti];
    RECT rc;
    rc.top = col;               // LVM_GETSUBITEMRECT：top 填列号
    rc.left = LVIR_BOUNDS;      // left 填 LVIR_BOUNDS
    rc.right = 0;
    rc.bottom = 0;
    SendMessageW(g_hList, LVM_GETSUBITEMRECT, row, (LPARAM)&rc);   // wParam=行号
    InflateRect(&rc, -2, -2);
    if (rc.right <= rc.left || rc.bottom <= rc.top) { doEditTask(); return; }
    g_ipRow = row; g_ipCol = col; g_ipCancel = false;
    bool isCombo = (col == 1);
    DWORD style = WS_CHILD | WS_VISIBLE |
                  (isCombo ? (CBS_DROPDOWNLIST | WS_VSCROLL) : ES_AUTOHSCROLL);
    g_ipEdit = CreateWindowExW(WS_EX_CLIENTEDGE, isCombo ? L"COMBOBOX" : L"EDIT", nullptr,
        style, rc.left, rc.top, rc.right - rc.left, isCombo ? 140 : rc.bottom - rc.top,
        g_hList, nullptr, g_hInst, nullptr);
    if (!g_ipEdit) { doEditTask(); return; }
    SendMessageW(g_ipEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (isCombo) {
        for (int i = 0; i < (int)subjects.size(); ++i)
            SendMessageW(g_ipEdit, CB_ADDSTRING, 0, (LPARAM)subjects[i].name.c_str());
        SendMessageW(g_ipEdit, CB_SETCURSEL, si, 0);
    } else {
        SetWindowTextW(g_ipEdit, t.text.c_str());
        SendMessageW(g_ipEdit, EM_SETSEL, 0, -1);
    }
    g_ipEditOrig = (WNDPROC)SetWindowLongPtrW(g_ipEdit, GWLP_WNDPROC, (LONG_PTR)IPEditProc);
    SetFocus(g_ipEdit);
}

// ==================== 菜单 ====================
static void AppendItem(HMENU m, UINT id, const wstring& txt, bool check = false) {
    AppendMenuW(m, MF_STRING | (check ? MF_CHECKED : 0), id, txt.c_str());
}
static HMENU BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU f = CreatePopupMenu();
    AppendItem(f, IDM_NEW, L"新建作业单\tCtrl+N");
    AppendItem(f, IDM_OPEN, L"打开...\tCtrl+O");
    AppendItem(f, IDM_SAVE, L"保存\tCtrl+S");
    AppendItem(f, IDM_SAVEAS, L"另存为...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendItem(f, IDM_OPENFOLDER, L"打开 homework 文件夹");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendItem(f, IDM_EXIT, L"退出");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)f, L"文件(&F)");

    HMENU t = CreatePopupMenu();
    AppendItem(t, IDM_ADDSUBJ, L"新增科目...");
    AppendItem(t, IDM_RENSUBJ, L"重命名科目...");
    AppendItem(t, IDM_DELSUBJ, L"删除科目...");
    AppendMenuW(t, MF_SEPARATOR, 0, nullptr);
    AppendItem(t, IDM_ADDTASK, L"新增任务...\tCtrl+T");
    AppendItem(t, IDM_EDITTASK, L"编辑任务...\tF2");
    AppendItem(t, IDM_DELTASK, L"删除任务\tDel");
    AppendItem(t, IDM_TOGGLE, L"标记完成/取消\tSpace");
    AppendItem(t, IDM_INSBEFORE, L"在前面插入任务...");
    AppendItem(t, IDM_INSAFTER, L"在后面插入任务...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)t, L"作业(&J)");

    HMENU v = CreatePopupMenu();
    AppendItem(v, IDM_VIEWALL, L"全部任务", viewMode == VM_ALL);
    AppendItem(v, IDM_VIEWUNDONE, L"未完成", viewMode == VM_UNDONE);
    AppendItem(v, IDM_VIEWDONE, L"已完成", viewMode == VM_DONE);
    AppendItem(v, IDM_VIEWSUBJ, L"按科目", viewMode == VM_SUBJECT);
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendItem(v, IDM_SEARCH, L"搜索...\tCtrl+F");
    AppendItem(v, IDM_CLEARSEARCH, L"清除搜索");
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendItem(v, IDM_SORTORIG, L"排序：原始", sortMode == S_ORIG);
    AppendItem(v, IDM_SORTSUBJ, L"排序：科目", sortMode == S_SUBJECT);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)v, L"视图(&V)");

    HMENU tt = CreatePopupMenu();
    AppendItem(tt, IDM_POMO, L"番茄钟");
    AppendItem(tt, IDM_STATS, L"进度统计");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tt, L"工具(&T)");

    HMENU th = CreatePopupMenu();
    AppendItem(th, IDM_THEME, darkTheme ? L"切换到浅色主题" : L"切换到深色主题");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)th, L"主题(&M)");

    HMENU hp = CreatePopupMenu();
    AppendItem(hp, IDM_ABOUT, L"关于");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)hp, L"帮助(&H)");
    return bar;
}

// ==================== 深色/浅色主题应用 ====================
static void applyTheme() {
    // 主窗口背景
    InvalidateRect(g_hMain, nullptr, TRUE);
    // 进度条颜色
    if (darkTheme) {
        SendMessageW(g_hProgress, PBM_SETBARCOLOR, 0, (LPARAM)RGB(70,140,220));
        SendMessageW(g_hProgress, PBM_SETBKCOLOR, 0, (LPARAM)RGB(50,50,56));
    } else {
        SendMessageW(g_hProgress, PBM_SETBARCOLOR, 0, (LPARAM)RGB(0,120,215));
        SendMessageW(g_hProgress, PBM_SETBKCOLOR, 0, (LPARAM)RGB(240,240,240));
    }
    SendMessageW(g_hList, LVM_SETBKCOLOR, 0, (LPARAM)bgColor());
    SendMessageW(g_hList, LVM_SETTEXTCOLOR, 0, (LPARAM)textColor());
    SendMessageW(g_hList, LVM_SETTEXTBKCOLOR, 0, (LPARAM)bgColor());
    SendMessageW(g_hTree, TVM_SETBKCOLOR, 0, (LPARAM)bgColor());
    SendMessageW(g_hTree, TVM_SETTEXTCOLOR, 0, (LPARAM)textColor());
    // 深色模式下 ListView 列头仍为系统色，可接受
    InvalidateRect(g_hList, nullptr, TRUE);
    InvalidateRect(g_hTree, nullptr, TRUE);
}

// ==================== 主窗口过程 ====================
static void layout(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    const int TOP = 44;
    const int BOT = 56;                       // 底部预留：进度条 + 统计 + 状态栏
    const int progW = 180;
    // 进度条：底部左段
    MoveWindow(g_hProgress, 10, H - BOT + 8, progW, 18, TRUE);
    // 统计文本：进度条右侧，宽度自适应，与进度条/状态栏均不重叠
    MoveWindow(g_hStatText, progW + 24, H - BOT + 4, W - progW - 34, 22, TRUE);
    MoveWindow(g_hTree, 0, TOP, 230, H - TOP - BOT, TRUE);
    MoveWindow(g_hList, 234, TOP, W - 234, H - TOP - BOT, TRUE);
}

static void applyTreeFilter(int lp) {
    if (lp == VM_ALL || lp == VM_UNDONE || lp == VM_DONE) {
        viewMode = (ViewMode)lp; viewSubject = -1;
    } else if (lp >= VM_SUBJECT * 10000) {
        viewMode = VM_SUBJECT; viewSubject = lp % 10000;
    }
    rebuildList(); updateStatus();
    // 更新菜单勾选
    HMENU bar = GetMenu(g_hMain);
    HMENU v = GetSubMenu(bar, 2);
    CheckMenuItem(v, IDM_VIEWALL, MF_BYCOMMAND | (viewMode==VM_ALL?MF_CHECKED:MF_UNCHECKED));
    CheckMenuItem(v, IDM_VIEWUNDONE, MF_BYCOMMAND | (viewMode==VM_UNDONE?MF_CHECKED:MF_UNCHECKED));
    CheckMenuItem(v, IDM_VIEWDONE, MF_BYCOMMAND | (viewMode==VM_DONE?MF_CHECKED:MF_UNCHECKED));
    CheckMenuItem(v, IDM_VIEWSUBJ, MF_BYCOMMAND | (viewMode==VM_SUBJECT?MF_CHECKED:MF_UNCHECKED));
}

static void setSort(SortMode sm) {
    sortMode = sm;
    HMENU bar = GetMenu(g_hMain);
    HMENU v = GetSubMenu(bar, 2);
    CheckMenuItem(v, IDM_SORTORIG, MF_BYCOMMAND | (sortMode==S_ORIG?MF_CHECKED:MF_UNCHECKED));
    CheckMenuItem(v, IDM_SORTSUBJ, MF_BYCOMMAND | (sortMode==S_SUBJECT?MF_CHECKED:MF_UNCHECKED));
    rebuildList();
}

static void doDeleteTask() {
    int si, ti;
    if (!getSelected(si, ti)) { MessageBoxW(g_hMain, L"请先选择要删除的任务。", L"提示", MB_OK); return; }
    wstring q = L"确定删除任务：【" + subjects[si].tasks[ti].text + L"】？";
    if (MessageBoxW(g_hMain, q.c_str(), L"删除", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    subjects[si].tasks.erase(subjects[si].tasks.begin() + ti);
    if (subjects[si].tasks.empty()) {
        if (MessageBoxW(g_hMain, (L"科目【" + subjects[si].name + L"】已无任务，是否同时删除该科目？").c_str(),
                        L"提示", MB_YESNO) == IDYES)
            subjects.erase(subjects.begin() + si);
    }
    rebuildTree(); rebuildList(); updateStatus(); autoSave();
}

static void doToggle() {
    int si, ti;
    if (!getSelected(si, ti)) { MessageBoxW(g_hMain, L"请先选择一个任务。", L"提示", MB_OK); return; }
    subjects[si].tasks[ti].done = !subjects[si].tasks[ti].done;
    rebuildList(); updateStatus(); autoSave();
}

static void doInsert(bool after) {
    int si, ti;
    if (!getSelected(si, ti)) { MessageBoxW(g_hMain, L"请先选择一个任务作为插入位置。", L"提示", MB_OK); return; }
    Task t;
    g_editSubj = si; g_editIdx = -1;
    g_insertMode = true;
    g_insertPos = after ? ti + 1 : ti;
    HWND d = CreateWindowW(L"TTaskDlg", L"插入任务", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 380, 260, g_hMain, nullptr, g_hInst, &t);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-380)/2, r.top + (r.bottom-r.top-260)/2, 380, 260, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    g_insertMode = false;
    rebuildTree(); rebuildList(); updateStatus(); autoSave();
}

static void doSearch() {
    HWND d = CreateWindowW(L"TSearchDlg", L"搜索", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 300, 130, g_hMain, nullptr, g_hInst, nullptr);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-300)/2, r.top + (r.bottom-r.top-130)/2, 300, 130, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildList(); updateStatus();
}

static LRESULT CALLBACK SearchDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        AddLabel(h, L"关键词（可匹配任务/标签）：", 14, 16, 260, 20);
        HWND e = AddEdit(h, IDC_SEARCH_EDIT, searchWord, 14, 42, 260, 26);
        AddButton(h, IDOK, L"确定", 60, 82, 80, 30);
        AddButton(h, IDCANCEL, L"取消", 160, 82, 80, 30);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[256];
            GetDlgItemTextW(h, IDC_SEARCH_EDIT, buf, 256);
            searchWord = trimW(buf);
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void doSaveAs() {
    static wstring init;
    init = currentName;
    HWND d = CreateWindowW(L"TSaveAsDlg", L"另存为", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                           0, 0, 320, 140, g_hMain, nullptr, g_hInst, &init);
    RECT r{}; GetWindowRect(g_hMain, &r);
    SetWindowPos(d, nullptr, r.left + (r.right-r.left-320)/2, r.top + (r.bottom-r.top-140)/2, 320, 140, SWP_NOZORDER);
    ShowWindow(d, SW_SHOW); UpdateWindow(d);
    ModalLoop(d);
    rebuildTree(); updateStatus();
}

static LRESULT CALLBACK SaveAsDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        wstring* init = (wstring*)((CREATESTRUCTW*)l)->lpCreateParams;
        AddLabel(h, L"作业单名称（保存到 homework 文件夹）：", 14, 16, 280, 20);
        HWND e = AddEdit(h, IDC_SUBJ_NAME, *init, 14, 42, 280, 26);
        AddButton(h, IDOK, L"保存", 70, 84, 80, 30);
        AddButton(h, IDCANCEL, L"取消", 170, 84, 80, 30);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[256];
            GetDlgItemTextW(h, IDC_SUBJ_NAME, buf, 256);
            wstring name = trimW(buf);
            if (name.empty()) { MessageBoxW(h, L"名称不能为空。", L"提示", MB_OK); return 0; }
            name = safeFileName(name);
            if (writeHomeworkFile(name)) {
                currentName = name;
                DestroyWindow(h);
            } else {
                MessageBoxW(h, L"保存失败。", L"错误", MB_OK);
            }
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        // 字体
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_fontBold = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        // 菜单
        SetMenu(h, BuildMenu());

        // 工具栏按钮
        struct Btn { int id; const wchar_t* txt; };
        Btn btns[] = {
            {IDM_NEW, L"新建"}, {IDM_OPEN, L"打开"}, {IDM_SAVE, L"保存"},
            {0, L"|"},
            {IDM_ADDSUBJ, L"加科目"}, {IDM_ADDTASK, L"加任务"}, {IDM_EDITTASK, L"编辑"},
            {IDM_DELTASK, L"删除"}, {IDM_TOGGLE, L"标记"},
            {0, L"|"},
            {IDM_SEARCH, L"搜索"}, {IDM_POMO, L"番茄钟"}, {IDM_STATS, L"统计"}
        };
        int x = 6;
        for (auto& b : btns) {
            if (b.txt[0] == L'|') { x += 6; continue; }
            AddButton(h, b.id, b.txt, x, 8, 60, 28);
            x += 66;
        }

        // 科目树
        g_hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT,
            0, 44, 230, 300, h, (HMENU)(INT_PTR)ID_TREE, g_hInst, nullptr);
        // 任务列表
        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            234, 44, 600, 300, h, (HMENU)(INT_PTR)ID_LIST, g_hInst, nullptr);
        // 列
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* heads[] = {L"状态", L"学科", L"任务"};
        int widths[] = {64, 84, 560};
        for (int i = 0; i < 3; ++i) {
            col.iSubItem = i; col.pszText = (LPWSTR)heads[i]; col.cx = widths[i];
            ListView_InsertColumn(g_hList, i, &col);
        }
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_hTree, WM_SETFONT, (WPARAM)g_font, TRUE);

        // 状态栏
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, h, nullptr, g_hInst, nullptr);
        // 进度条 + 统计文本
        g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
                                      8, 300, 200, 18, h, (HMENU)(INT_PTR)ID_PROGRESS, g_hInst, nullptr);
        g_hStatText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                      216, 300, 260, 22, h, (HMENU)(INT_PTR)ID_STATTEXT, g_hInst, nullptr);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_hProgress, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_hStatText, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        applyTheme();
        // 加载最近的作业单（若存在 国庆2026.txt）
        auto files = listHomeworkFiles();
        if (!files.empty()) {
            wstring pick = L"国庆2026.txt";
            bool found = false;
            for (auto& f : files) if (f == pick) { found = true; break; }
            if (!found) pick = files.back();
            if (readHomeworkFile(pick)) {
                if (pick.size() > 4) pick.erase(pick.size() - 4);
                currentName = pick;
            }
        }
        rebuildTree(); rebuildList(); updateStatus();
        return 0;
    }

    case WM_SIZE:
        layout(h);
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HWND ctrl = (HWND)l;
        if (ctrl == g_hStatText) {
            HDC dc = (HDC)w;
            SetTextColor(dc, darkTheme ? RGB(230,230,230) : RGB(20,20,20));
            // 用不透明背景：文字更新时能清掉旧字，避免重叠残影
            SetBkMode(dc, OPAQUE);
            if (darkTheme) {
                SetBkColor(dc, RGB(30,30,34));
                static HBRUSH brD = CreateSolidBrush(RGB(30,30,34));
                return (LRESULT)brD;
            } else {
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
        }
        break;
    }

    case WM_ERASEBKGND:
        if (darkTheme) {
            HDC dc = (HDC)w;
            RECT rc; GetClientRect(h, &rc);
            HBRUSH br = CreateSolidBrush(RGB(30,30,34));
            FillRect(dc, &rc, br);
            DeleteObject(br);
            return 1;
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(w);
        switch (id) {
        case IDM_NEW:
            if (MessageBoxW(h, L"新建将清空当前内容（未保存数据会丢失），继续？", L"新建", MB_YESNO | MB_ICONWARNING) == IDYES)
                resetData();
            break;
        case IDM_OPEN: doOpen(); break;
        case IDM_SAVE:
            if (currentName.empty()) doSaveAs();
            else { writeHomeworkFile(currentName); rebuildTree(); updateStatus(); }
            break;
        case IDM_SAVEAS: doSaveAs(); break;
        case IDM_OPENFOLDER: {
            wstring cmd = L"explorer.exe \"" + HW_DIR + L"\"";
            _wsystem(cmd.c_str());
            break;
        }
        case IDM_EXIT: DestroyWindow(h); break;
        case IDM_ADDSUBJ: doAddSubject(); break;
        case IDM_RENSUBJ: doRenameSubject(); break;
        case IDM_DELSUBJ: doDeleteSubject(); break;
        case IDM_ADDTASK: doAddTask(); break;
        case IDM_EDITTASK: doEditTask(); break;
        case IDM_DELTASK: doDeleteTask(); break;
        case IDM_TOGGLE: doToggle(); break;
        case IDM_INSBEFORE: doInsert(false); break;
        case IDM_INSAFTER: doInsert(true); break;
        case IDM_VIEWALL: applyTreeFilter(VM_ALL); break;
        case IDM_VIEWUNDONE: applyTreeFilter(VM_UNDONE); break;
        case IDM_VIEWDONE: applyTreeFilter(VM_DONE); break;
        case IDM_VIEWSUBJ: applyTreeFilter(VM_SUBJECT); break;
        case IDM_SEARCH: doSearch(); rebuildList(); updateStatus(); break;
        case IDM_CLEARSEARCH: searchWord.clear(); rebuildList(); updateStatus(); break;
        case IDM_SORTORIG: setSort(S_ORIG); break;
        case IDM_SORTSUBJ: setSort(S_SUBJECT); break;
        case IDM_THEME:
            darkTheme = !darkTheme;
            {
                HMENU bar = GetMenu(h);
                HMENU th = GetSubMenu(bar, 4);
                ModifyMenuW(th, IDM_THEME, MF_BYCOMMAND | MF_STRING, IDM_THEME,
                            darkTheme ? L"切换到浅色主题" : L"切换到深色主题");
                DrawMenuBar(h);
            }
            applyTheme();
            break;
        case IDM_POMO: doPomo(); break;
        case IDM_STATS: doStats(); break;
        case IDM_ABOUT:
            MessageBoxW(h, L"作业待办 v4（图形桌面版）\n\n在 v2 基础上升级：\n"
                        L"· 图形界面：科目树 + 任务列表 + 进度条\n"
                        L"· 单击状态列打勾，双击任务/学科列就地编辑\n"
                        L"· 搜索与多视图过滤、列排序\n"
                        L"· 番茄钟、统计面板、深色主题\n"
                        L"· 自动保存，兼容读取 v2 数据文件",
                        L"关于", MB_OK);
            break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)l;
        if (hdr->idFrom == ID_TREE && hdr->code == TVN_SELCHANGED) {
            NMTREEVIEWW* ntv = (NMTREEVIEWW*)l;
            if (ntv->itemNew.lParam != -1) applyTreeFilter((int)ntv->itemNew.lParam);
        }
        else if (hdr->idFrom == ID_LIST) {
            if (hdr->code == LVN_COLUMNCLICK) {
                NMLISTVIEW* nlv = (NMLISTVIEW*)l;
                // 0状态 1学科 2任务
                switch (nlv->iSubItem) {
                case 0: setSort(S_ORIG); break;
                case 1: setSort(S_SUBJECT); break;
                case 2: setSort(S_ORIG); break;
                }
            }
            else if (hdr->code == NM_CLICK) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(g_hList, &pt);
                LVHITTESTINFO hit{};
                hit.pt = pt;
                int row = ListView_SubItemHitTest(g_hList, &hit);
                if (row >= 0 && hit.iSubItem <= 0) doToggle();   // 单击状态列 = 打勾/取消
            }
            else if (hdr->code == NM_DBLCLK) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(g_hList, &pt);
                LVHITTESTINFO hit{};
                hit.pt = pt;
                int row = ListView_SubItemHitTest(g_hList, &hit);
                if (row >= 0) {
                    int col = hit.iSubItem;
                    if (col <= 0) doToggle();                      // 状态列：切换完成
                    else if (col == 1) ipStartEdit(row, 1);       // 学科列：下拉换科目
                    else ipStartEdit(row, 2);                     // 任务列：就地编辑（定位失败也默认编辑任务）
                }
            }
            else if (hdr->code == NM_RCLICK) {
                int si, ti;
                if (getSelected(si, ti)) {
                    POINT pt; GetCursorPos(&pt);
                    HMENU pop = CreatePopupMenu();
                    AppendMenuW(pop, MF_STRING, IDM_TOGGLE, L"标记完成/取消");
                    AppendMenuW(pop, MF_STRING, IDM_EDITTASK, L"编辑...");
                    AppendMenuW(pop, MF_STRING, IDM_DELTASK, L"删除");
                    AppendMenuW(pop, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(pop, MF_STRING, IDM_ADDTASK, L"新增任务...");
                    int cmd = TrackPopupMenu(pop, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                    DestroyMenu(pop);
                    if (cmd == IDM_TOGGLE) doToggle();
                    else if (cmd == IDM_EDITTASK) doEditTask();
                    else if (cmd == IDM_DELTASK) doDeleteTask();
                    else if (cmd == IDM_ADDTASK) doAddTask();
                }
            }
            else if (hdr->code == NM_CUSTOMDRAW) {
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)l;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int row = (int)cd->nmcd.dwItemSpec;
                    if (row >= 0 && row < (int)g_visible.size()) {
                        int si = g_visible[row].first;
                        const Task& t = subjects[si].tasks[g_visible[row].second];
                        bool sel = (ListView_GetItemState(g_hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        if (sel) {
                            cd->clrText = RGB(255,255,255);   // 选中：系统高亮 + 白字
                        } else {
                            cd->clrTextBk = subjBgColor(si);      // 背景统一用学科色
                            cd->clrText = t.done ? doneColor() : textColor();  // 完成=绿字标记
                        }
                        return CDRF_NEWFONT;
                    }
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        if (g_fontBold) DeleteObject(g_fontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ==================== 自检模式 ====================
static int runSelfTest() {
    // 读取指定作业单，把统计写入 selftest_out.txt（GBK），然后退出
    auto files = listHomeworkFiles();
    wostringstream os;
    os << L"文件数=" << (int)files.size() << L"\n";
    for (auto& f : files) {
        if (readHomeworkFile(f)) {
            os << L"[" << f << L"] 科目=" << (int)subjects.size()
               << L" 任务=" << totalTasks() << L" 完成=" << totalDone()
               << L" 完成率=" << (totalTasks()?totalDone()*100/totalTasks():0) << L"%\n";
            for (auto& s : subjects)
                os << L"  - " << s.name << L": " << (int)s.tasks.size() << L" 项\n";
        } else {
            os << L"[" << f << L"] 读取失败\n";
        }
    }
    string gbk = wideToGBK(os.str());
    { ofstream out("selftest_out.txt", ios::binary); out.write(gbk.data(), (streamsize)gbk.size()); }

    // 写一个测试文件再读回
    subjects.clear();
    subjects.push_back(Subject{L"语文", {}});
    Task t1; t1.text = L"背古诗"; t1.priority = 2; t1.due = L"2026-10-05"; t1.tag = L"背诵";
    subjects[0].tasks.push_back(t1);
    Task t2; t2.text = L"默写"; t2.done = true;
    subjects[0].tasks.push_back(t2);
    if (writeHomeworkFile(L"_v3selftest")) {
        subjects.clear();
        if (readHomeworkFile(L"_v3selftest.txt")) {
            wostringstream os2;
            os2 << L"科目=" << (int)subjects.size() << L" 任务=" << totalTasks() << L" 完成=" << totalDone() << L"\n";
            os2 << L"任务1文本=" << subjects[0].tasks[0].text
                << L" pri=" << subjects[0].tasks[0].priority
                << L" due=" << subjects[0].tasks[0].due
                << L" tag=" << subjects[0].tasks[0].tag << L"\n";
            os2 << L"任务2完成=" << (subjects[0].tasks[1].done ? L"是" : L"否") << L"\n";
            string g2 = wideToGBK(os2.str());
            ofstream out2("selftest_roundtrip.txt", ios::binary);
            out2.write(g2.data(), (streamsize)g2.size());
        }
    }
    return 0;
}

// ==================== 入口 ====================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    // 自检模式：命令行带 --selftest
    wstring cmd = pCmdLine ? pCmdLine : L"";
    if (cmd.find(L"--selftest") != wstring::npos) return runSelfTest();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // 注册窗口类
    struct { const wchar_t* name; WNDPROC proc; } classes[] = {
        {L"TTaskDlg", TaskDlgProc},
        {L"TSubjectDlg", SubjectDlgProc},
        {L"TOpenDlg", OpenDlgProc},
        {L"TPomoDlg", PomoDlgProc},
        {L"TStatsDlg", StatsDlgProc},
        {L"TSearchDlg", SearchDlgProc},
        {L"TSaveAsDlg", SaveAsDlgProc},
    };
    for (auto& c : classes) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = c.proc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = c.name;
        RegisterClassW(&wc);
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"StudyTodoV4";
    RegisterClassW(&wc);

    g_hMain = CreateWindowW(L"StudyTodoV4", L"作业待办 v4",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 620,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hMain) return 0;
    ShowWindow(g_hMain, nCmdShow);
    UpdateWindow(g_hMain);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
