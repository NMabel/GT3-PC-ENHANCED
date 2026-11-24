/* save_manager.cpp - the save manager (save_manager.h).
 *
 * PC (2026-11-15): the texture manager's window (texture_manager.cpp) - its
 * fonts, its DPI handling, its keys - over two lists: the engine's save
 * folders and the selected folder's saves (save_card.h), their icons drawn
 * by ps2_icon.h. */
#include "save_manager.h"

#include "platform/save_folders.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */
#include "ps2_icon.h"
#include "psu.h"
#include "save_card.h"
#include "util.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <map>
#include <vector>

namespace save_manager {
namespace {

constexpr const wchar_t *kClass = L"GT3LauncherSaveManager";
constexpr const wchar_t *kTitle = N_(L"Saves");     /* (shown as tr(kTitle)) */

/* Layout, in 96-DPI units. */
constexpr int kW = 960, kH = 660, kM = 12, kGap = 8, kBtnH = 28, kHintH = 36, kPathH = 18;
constexpr int kSlotLabelW = 118, kSlotComboW = 230, kLeftW = 330, kLabelH = 20, kStatsH = 84, kDetailsH = 84;
constexpr int kCloseW = 110, kIcon = 32;

enum : int {
    IDC_HINT = 3201,
    IDC_PATH,
    IDC_SLOT_LABEL1,
    IDC_SLOT_LABEL2,
    IDC_SLOT1,
    IDC_SLOT2,
    IDC_FOLDERS_LABEL,
    IDC_FOLDERS,
    IDC_NEW,
    IDC_RENAME,
    IDC_CLEAR,
    IDC_DELETE,
    IDC_STATS,
    IDC_CONTENTS,
    IDC_DETAILS,
    IDC_COPY,
    IDC_DELETE_SAVES,
    IDC_IMPORT,
    IDC_EXPORT,
    IDC_STATUS,
    IDC_COPY_TO0 = 3300,                    /* the Copy to... menu's folders */
};

struct Folder {
    std::wstring name;
    int kb = 0, saves = 0;
};

struct Manager {
    HWND wnd = nullptr, owner = nullptr;
    HWND hint = nullptr, path = nullptr, slotLabel[2] = {}, slot[2] = {}, foldersLabel = nullptr, folders = nullptr,
         newFolder = nullptr, rename = nullptr, clear = nullptr, remove = nullptr, stats = nullptr,
         contents = nullptr, details = nullptr, copy = nullptr, removeSaves = nullptr, importPsu = nullptr,
         exportPsu = nullptr, status = nullptr, close = nullptr;
    HFONT font = nullptr;
    HIMAGELIST images = nullptr;
    UINT dpi = 96;
    std::wstring dir, ini, engine;
    void (*changed)() = nullptr;
    std::vector<Folder> list;               /* the folders listed */
    std::wstring current;                   /* the folder shown at the right ("" none) */
    save_card::Card card;
    std::vector<std::wstring> copyTargets;  /* the Copy to... menu's folders */
    bool filling = false;
    /* an operation under way (its questions and messages): the window's
     * activation does not read the folders again under it */
    int quiet = 0;
    bool showPosted = false;                /* (a folder's selection changed: show_card posted) */
};

Manager g;

struct Quiet {
    Quiet() { g.quiet++; }
    ~Quiet() { g.quiet--; }
};

constexpr UINT WM_APP_RENAMED = WM_APP, WM_APP_SHOW = WM_APP + 1;
/* the icons drawn, by their files' bytes and the size (kept while the
 * launcher runs: the same few icons in every folder) */
std::map<uint64_t, std::vector<uint32_t>> g_icons;

int px(int v) { return MulDiv(v, (int)g.dpi, 96); }

void set_status(const std::wstring &t) { SetWindowTextW(g.status, t.c_str()); }

std::wstring folder_path(const std::wstring &name) { return g.dir + name; }

bool same(const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

void notify()
{
    if (g.changed != nullptr)
        g.changed();
}

/* The slot (0, 1) holding the folder, -1 none. */
int slot_of(const std::wstring &name)
{
    for (int s = 0; s < save_folders::kSlots; s++) {
        const std::wstring f = save_folders::slot(g.ini, s);
        if (!f.empty() && same(f, name))
            return s;
    }
    return -1;
}

std::vector<uint8_t> read_file(const std::wstring &p)
{
    std::vector<uint8_t> d;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return d;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= 0x200000) {   /* (T108's largest is 0x195D4C) */
        d.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, d.data(), (DWORD)d.size(), &got, nullptr))
            got = 0;
        d.resize(got);
    }
    CloseHandle(h);
    return d;
}

uint64_t fnv(uint64_t h, const std::vector<uint8_t> &d)
{
    for (uint8_t b : d)
        h = (h ^ b) * 0x100000001B3ULL;
    return h;
}

/* A save's icon at the list's size (empty: none). */
const std::vector<uint32_t> *icon_of(const save_card::Save &s)
{
    if (s.icon.empty())
        return nullptr;
    const std::vector<uint8_t> ico = read_file(s.icon);
    if (ico.empty())
        return nullptr;
    const std::vector<uint8_t> sys = read_file(folder_path(g.current) + L"\\" + s.dir + L"\\icon.sys");
    const int size = px(kIcon);
    const uint64_t key = fnv(fnv(0xCBF29CE484222325ULL ^ (uint64_t)size, ico), sys);
    auto it = g_icons.find(key);
    if (it == g_icons.end()) {
        std::vector<uint32_t> pixels;
        if (!ps2_icon::render(ico, sys.empty() ? nullptr : &sys, size, &pixels))
            pixels.clear();
        it = g_icons.emplace(key, std::move(pixels)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

/* An image list's picture from premultiplied BGRA (nullptr: a clear one). */
int add_image(HIMAGELIST list, const std::vector<uint32_t> *pixels)
{
    const int size = px(kIcon);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (bmp == nullptr)
        return -1;
    if (pixels != nullptr && pixels->size() == (size_t)size * size)
        memcpy(bits, pixels->data(), pixels->size() * 4);
    else
        memset(bits, 0, (size_t)size * size * 4);
    const int i = ImageList_Add(list, bmp, nullptr);
    DeleteObject(bmp);
    return i;
}

void set_cell(HWND list, int row, int col, const std::wstring &text)
{
    LVITEMW it = {};
    it.iSubItem = col;
    it.pszText = const_cast<wchar_t *>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

void select_row(HWND list, int row)
{
    LVITEMW it = {};
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
    SendMessageW(list, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
}

int selected_folder()
{
    const int i = (int)SendMessageW(g.folders, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return i >= 0 && i < (int)g.list.size() ? i : -1;
}

std::vector<int> selected_saves()
{
    std::vector<int> out;
    for (int i = -1; (i = (int)SendMessageW(g.contents, LVM_GETNEXTITEM, (WPARAM)i, LVNI_SELECTED)) >= 0;)
        if (i < (int)g.card.saves.size())
            out.push_back(i);
    return out;
}

bool editing() { return g.folders != nullptr && ListView_GetEditControl(g.folders) != nullptr; }

/* ---- the right side: the selected folder ---- */

void buttons()
{
    const int f = selected_folder();
    EnableWindow(g.rename, f >= 0);
    EnableWindow(g.clear, f >= 0 && g.list[(size_t)f].saves > 0);
    EnableWindow(g.remove, f >= 0);
    const bool saves = !selected_saves().empty();
    EnableWindow(g.copy, saves && g.list.size() > 1);
    EnableWindow(g.removeSaves, saves);
    EnableWindow(g.importPsu, f >= 0);
    EnableWindow(g.exportPsu, saves);
}

void show_details()
{
    const std::vector<int> sel = selected_saves();
    std::wstring t;
    if (sel.size() == 1) {
        t = save_card::details(g.card.saves[(size_t)sel[0]]);
    } else if (sel.size() > 1) {
        int kb = 0;
        for (int i : sel)
            kb += g.card.saves[(size_t)i].kb;
        t = trf(L"%d saves selected, %s KB.", (int)sel.size(), save_card::number(kb).c_str());
    }
    SetWindowTextW(g.details, t.c_str());
    buttons();
}

std::wstring stats_text()
{
    if (g.current.empty())
        return g.list.empty() ? tr(L"No save folders yet. New makes one: a blank memory card.") : L"";
    if (!g.card.there)
        return trf(L"%s is not there any more.", g.current.c_str());
    const size_t n = g.card.saves.size();
    /* (the count as a phrase of its own: "no saves", "1 save", "12 saves") */
    const std::wstring count = n == 0   ? std::wstring(tr(L"no saves"))
                               : n == 1 ? std::wstring(tr(L"1 save"))
                                        : trf(L"%s saves", save_card::number((long long)n).c_str());
    const std::wstring usedKB = save_card::number(g.card.usedKB), cardKB = save_card::number(save_card::kCardKB),
                       freeKB = save_card::number(g.card.freeKB());
    const int s = slot_of(g.current);
    std::wstring t =
        s >= 0 ? trf(L"%s: %s, %s KB used of %s KB (%s KB free), in memory card slot %d.", g.current.c_str(),
                     count.c_str(), usedKB.c_str(), cardKB.c_str(), freeKB.c_str(), s + 1)
               : trf(L"%s: %s, %s KB used of %s KB (%s KB free).", g.current.c_str(), count.c_str(), usedKB.c_str(),
                     cardKB.c_str(), freeKB.c_str());
    if (n > 0) {
        std::wstring kinds;
        for (save_card::Kind k : {save_card::Kind::game, save_card::Kind::replay, save_card::Kind::bestLap,
                                  save_card::Kind::licence, save_card::Kind::setting, save_card::Kind::other}) {
            const long long c = std::count_if(g.card.saves.begin(), g.card.saves.end(),
                                              [k](const save_card::Save &v) { return v.kind == k; });
            if (c > 0)
                kinds += (kinds.empty() ? L"" : L"  \x00B7  ") + save_card::kind_name(k, c > 1) + L": " +
                         save_card::number(c);
        }
        t += L"\r\n" + kinds;
        for (const save_card::Save &v : g.card.saves)
            if (v.kind == save_card::Kind::game) {
                const std::wstring progress = save_card::game_summary(v);
                if (!progress.empty())
                    t += L"\r\n" +
                         (v.region.empty() ? std::wstring(tr(L"Game data"))
                                           : trf(L"Game data (%s)", tr(v.region.c_str()))) +
                         L": " + progress;
            }
    }
    return t;
}

void content_columns()
{
    /* (the widths fit beside a vertical scroll bar: no horizontal one when
     * the saves fill the list) */
    RECT rc;
    GetClientRect(g.contents, &rc);
    const int kind = px(150), size = px(62), date = px(132);
    const int title = std::max(px(120), (int)rc.right - GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi) - kind - size - date);
    const struct {
        const wchar_t *name;
        int width, fmt;
    } cols[] = {{N_(L"Save"), title, LVCFMT_LEFT}, {N_(L"Kind"), kind, LVCFMT_LEFT}, {N_(L"Size"), size, LVCFMT_RIGHT},
                {N_(L"Saved"), date, LVCFMT_LEFT}};
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t *>(tr(cols[i].name));
        col.cx = cols[i].width;
        col.fmt = cols[i].fmt;
        if (SendMessageW(g.contents, LVM_SETCOLUMNWIDTH, (WPARAM)i, (LPARAM)col.cx) == FALSE)
            SendMessageW(g.contents, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
}

/* The selected folder's statistics and saves; the saves selected before
 * (by their folders) kept selected. */
void show_card()
{
    std::vector<std::wstring> keep;
    for (int i : selected_saves())
        keep.push_back(g.card.saves[(size_t)i].dir);
    const int f = selected_folder();
    const std::wstring was = g.current;
    g.current = f >= 0 ? g.list[(size_t)f].name : std::wstring();
    if (!same(was, g.current))
        keep.clear();
    g.card = g.current.empty() ? save_card::Card() : save_card::read(folder_path(g.current));
    SetWindowTextW(g.stats, stats_text().c_str());

    g.filling = true;
    SendMessageW(g.contents, LVM_DELETEALLITEMS, 0, 0);
    HIMAGELIST images = ImageList_Create(px(kIcon), px(kIcon), ILC_COLOR32, (int)g.card.saves.size() + 1, 4);
    for (size_t i = 0; i < g.card.saves.size(); i++) {
        const save_card::Save &s = g.card.saves[i];
        const int image = add_image(images, icon_of(s));
        const std::wstring title = save_card::title(s);
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = (int)i;
        it.iImage = image;
        it.pszText = const_cast<wchar_t *>(title.c_str());
        SendMessageW(g.contents, LVM_INSERTITEMW, 0, (LPARAM)&it);
        set_cell(g.contents, (int)i, 1,
                 save_card::kind_name(s.kind) +
                     (s.region.empty() ? L"" : L" (" + std::wstring(tr(s.region.c_str())) + L")"));
        set_cell(g.contents, (int)i, 2, trf(L"%s KB", save_card::number(s.kb).c_str()));
        set_cell(g.contents, (int)i, 3, save_card::date_text(s.saved));
        if (std::any_of(keep.begin(), keep.end(), [&s](const std::wstring &k) { return same(k, s.dir); })) {
            LVITEMW st = {};
            st.stateMask = LVIS_SELECTED;
            st.state = LVIS_SELECTED;
            SendMessageW(g.contents, LVM_SETITEMSTATE, i, (LPARAM)&st);
        }
    }
    HIMAGELIST old = (HIMAGELIST)SendMessageW(g.contents, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)images);
    if (old != nullptr)
        ImageList_Destroy(old);
    g.images = images;
    g.filling = false;
    show_details();
}

/* ---- the left side: the folders and the slots ---- */

void fill_slots()
{
    g.filling = true;
    for (int s = 0; s < save_folders::kSlots; s++) {
        HWND c = g.slot[s];
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)tr(L"(No card)"));
        const std::wstring cur = save_folders::slot(g.ini, s), other = save_folders::slot(g.ini, 1 - s);
        int sel = 0;
        for (const Folder &f : g.list) {
            if (save_folders::name_problem(f.name) != nullptr || (!other.empty() && same(f.name, other)))
                continue;
            const int i = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)f.name.c_str());
            if (!cur.empty() && same(f.name, cur))
                sel = i;
        }
        if (!cur.empty() && sel == 0)       /* its folder is gone (or not usable): said so */
            sel = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)trf(L"%s (not found)", cur.c_str()).c_str());
        SendMessageW(c, CB_SETCURSEL, (WPARAM)sel, 0);
    }
    g.filling = false;
}

void folder_columns()
{
    RECT rc;
    GetClientRect(g.folders, &rc);
    const int slot = px(40), saves = px(50), used = px(72);
    const int name = std::max(px(100), (int)rc.right - GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi) - slot - saves - used);
    const struct {
        const wchar_t *name;
        int width, fmt;
    } cols[] = {{N_(L"Folder"), name, LVCFMT_LEFT}, {N_(L"Slot"), slot, LVCFMT_CENTER},
                {N_(L"Saves"), saves, LVCFMT_RIGHT}, {N_(L"Used"), used, LVCFMT_RIGHT}};
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t *>(tr(cols[i].name));
        col.cx = cols[i].width;
        col.fmt = cols[i].fmt;
        if (SendMessageW(g.folders, LVM_SETCOLUMNWIDTH, (WPARAM)i, (LPARAM)col.cx) == FALSE)
            SendMessageW(g.folders, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
}

int count_saves(const std::wstring &folder)
{
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") &&
            wcscmp(fd.cFileName, L".."))
            n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* Everything read again from the disk: the folders (`keep` selected, else
 * the one selected before, else the first), the slots, the selected
 * folder's saves. */
void refresh(std::wstring keep = std::wstring())
{
    if (keep.empty()) {
        const int f = selected_folder();
        keep = f >= 0 ? g.list[(size_t)f].name : g.current;
    }
    g.list.clear();
    for (const std::wstring &name : save_folders::list(g.dir)) {
        Folder f;
        f.name = name;
        f.kb = save_card::used_kb(folder_path(name));
        f.saves = count_saves(folder_path(name));
        g.list.push_back(f);
    }
    g.filling = true;
    SendMessageW(g.folders, LVM_DELETEALLITEMS, 0, 0);
    int sel = g.list.empty() ? -1 : 0;
    for (size_t i = 0; i < g.list.size(); i++) {
        const Folder &f = g.list[i];
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t *>(f.name.c_str());
        SendMessageW(g.folders, LVM_INSERTITEMW, 0, (LPARAM)&it);
        const int s = slot_of(f.name);
        set_cell(g.folders, (int)i, 1, s >= 0 ? std::to_wstring(s + 1) : std::wstring());
        set_cell(g.folders, (int)i, 2, save_card::number(f.saves));
        set_cell(g.folders, (int)i, 3, trf(L"%s KB", save_card::number(f.kb).c_str()));
        if (!keep.empty() && same(f.name, keep))
            sel = (int)i;
    }
    if (sel >= 0)
        select_row(g.folders, sel);
    g.filling = false;
    fill_slots();
    show_card();
}

std::wstring summary_line()
{
    const size_t n = g.list.size();
    return n == 0   ? std::wstring(tr(L"No save folders yet: New makes one."))
           : n == 1 ? trf(L"1 save folder in %s", g.dir.c_str())
                    : trf(L"%s save folders in %s", save_card::number((long long)n).c_str(), g.dir.c_str());
}

void slot_changed(int s)
{
    const int i = (int)SendMessageW(g.slot[s], CB_GETCURSEL, 0, 0);
    if (i < 0)
        return;
    wchar_t text[256] = L"";
    if (i > 0)
        SendMessageW(g.slot[s], CB_GETLBTEXT, (WPARAM)i, (LPARAM)text);
    std::wstring name = text;
    const std::wstring cur = save_folders::slot(g.ini, s);
    if (!cur.empty() && name == trf(L"%s (not found)", cur.c_str()))
        return;                             /* (the gone folder picked again: no change) */
    save_folders::set_slot(g.ini, s, name);
    refresh();
    set_status(name.empty() ? trf(L"Memory card slot %d: no card. The game takes it when it starts (in the game: "
                                  L"F2 > MEMORY CARDS).",
                                  s + 1)
                            : trf(L"Memory card slot %d: %s. The game takes it when it starts (in the game: "
                                  L"F2 > MEMORY CARDS).",
                                  s + 1, name.c_str()));
    notify();
}

/* ---- the folder operations ---- */

void begin_rename()
{
    const int f = selected_folder();
    if (f < 0)
        return;
    SetFocus(g.folders);
    SendMessageW(g.folders, LVM_EDITLABELW, (WPARAM)f, 0);
}

void new_folder()
{
    std::wstring name;
    for (int n = 1; n < 10000; n++) {
        name = L"New " + std::to_wstring(n);
        if (std::none_of(g.list.begin(), g.list.end(), [&name](const Folder &f) { return same(f.name, name); }) &&
            GetFileAttributesW(folder_path(name).c_str()) == INVALID_FILE_ATTRIBUTES)
            break;
    }
    if (!util::make_folders(folder_path(name))) {
        set_status(trf(L"Could not make %s: %s.", folder_path(name).c_str(),
                       util::system_error_text(GetLastError()).c_str()));
        return;
    }
    refresh(name);
    set_status(trf(L"Made %s, a blank memory card. Type its name, or Enter to keep this one.", name.c_str()));
    notify();
    begin_rename();
}

/* The label edit's end: the folder renamed (true: the list takes the text). */
bool end_rename(int row, const wchar_t *text)
{
    Quiet quiet;
    if (text == nullptr || row < 0 || row >= (int)g.list.size())
        return false;                       /* (cancelled) */
    const std::wstring from = g.list[(size_t)row].name, to = text;
    if (to == from)
        return false;
    if (const wchar_t *problem = save_folders::name_problem(to)) {
        /* (save_folders' text: translated here, also when it is marked N_ there) */
        MessageBoxW(g.wnd, (to + L"\n\n" + tr(problem)).c_str(), tr(kTitle), MB_OK | MB_ICONWARNING);
        return false;
    }
    for (const Folder &f : g.list)
        if (same(f.name, to) && !same(f.name, from)) {
            MessageBoxW(g.wnd, trf(L"There is a save folder named %s already.", f.name.c_str()).c_str(), tr(kTitle),
                        MB_OK | MB_ICONWARNING);
            return false;
        }
    if (!MoveFileW(folder_path(from).c_str(), folder_path(to).c_str())) {
        MessageBoxW(g.wnd,
                    trf(L"Could not rename %s:\n%s.\n\nIs the game using it?", from.c_str(),
                        util::system_error_text(GetLastError()).c_str())
                        .c_str(),
                    tr(kTitle), MB_OK | MB_ICONWARNING);
        return false;
    }
    for (int s = 0; s < save_folders::kSlots; s++)
        if (same(save_folders::slot(g.ini, s), from))
            save_folders::set_slot(g.ini, s, to);
    /* (refreshed after the list has taken the edit) */
    PostMessageW(g.wnd, WM_APP_RENAMED, 0, 0);
    g.current = to;
    set_status(trf(L"Renamed %s to %s.", from.c_str(), to.c_str()));
    return true;
}

void clear_folder()
{
    Quiet quiet;
    const int f = selected_folder();
    if (f < 0)
        return;
    const Folder fo = g.list[(size_t)f];
    if (fo.saves == 0)
        return;
    const std::wstring kb = save_card::number(fo.kb);
    const std::wstring q =
        (fo.saves == 1 ? trf(L"Delete every save in %s?\n\n1 save, %s KB. The folder stays, empty: a blank memory "
                             L"card.",
                             fo.name.c_str(), kb.c_str())
                       : trf(L"Delete every save in %s?\n\n%s saves, %s KB. The folder stays, empty: a blank memory "
                             L"card.",
                             fo.name.c_str(), save_card::number(fo.saves).c_str(), kb.c_str())) +
        L"\n\n" + tr(L"This cannot be undone.");
    if (MessageBoxW(g.wnd, q.c_str(), tr(kTitle), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    std::wstring error;
    const bool ok = save_card::remove_tree(folder_path(fo.name), false, &error);
    refresh(fo.name);
    set_status(ok ? trf(L"Cleared %s.", fo.name.c_str())
                  : trf(L"Not all of %s could be deleted: %s", fo.name.c_str(), error.c_str()));
    notify();
}

void delete_folder()
{
    Quiet quiet;
    const int f = selected_folder();
    if (f < 0)
        return;
    const Folder fo = g.list[(size_t)f];
    const int s = slot_of(fo.name);
    std::wstring q = trf(L"Delete the save folder %s and everything in it?", fo.name.c_str()) + L"\n\n";
    q += fo.saves == 0   ? std::wstring(tr(L"It has no saves."))
         : fo.saves == 1 ? trf(L"1 save, %s KB.", save_card::number(fo.kb).c_str())
                         : trf(L"%s saves, %s KB.", save_card::number(fo.saves).c_str(),
                               save_card::number(fo.kb).c_str());
    if (s >= 0)
        q += L"\n" + trf(L"Memory card slot %d will have no card.", s + 1);
    q += std::wstring(L"\n\n") + tr(L"This cannot be undone.");
    if (MessageBoxW(g.wnd, q.c_str(), tr(kTitle), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    std::wstring error;
    const bool ok = save_card::remove_tree(folder_path(fo.name), true, &error);
    if (ok)
        for (int k = 0; k < save_folders::kSlots; k++)
            if (same(save_folders::slot(g.ini, k), fo.name))
                save_folders::set_slot(g.ini, k, std::wstring());
    const int next = f + 1 < (int)g.list.size() ? f + 1 : f - 1;
    refresh(ok && next >= 0 && next < (int)g.list.size() ? g.list[(size_t)next].name : fo.name);
    set_status(ok ? trf(L"Deleted %s.", fo.name.c_str())
                  : trf(L"Not all of %s could be deleted: %s", fo.name.c_str(), error.c_str()));
    notify();
}

/* ---- the save operations ---- */

void copy_saves(const std::wstring &target)
{
    Quiet quiet;
    std::vector<save_card::Save> saves;
    for (int i : selected_saves())
        saves.push_back(g.card.saves[(size_t)i]);
    const std::wstring source = g.current;
    if (saves.empty() || target.empty())
        return;
    int copied = 0, skipped = 0;
    std::wstring problem;
    for (const save_card::Save &s : saves) {
        const std::wstring from = folder_path(source) + L"\\" + s.dir, to = folder_path(target) + L"\\" + s.dir;
        const bool there = GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (there) {
            const int a = MessageBoxW(g.wnd,
                                      trf(L"%s has this save already:\n%s\n(%s)\n\nReplace it with this one?",
                                          target.c_str(), save_card::title(s).c_str(), s.dir.c_str())
                                          .c_str(),
                                      tr(kTitle), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (a == IDCANCEL)
                break;
            if (a != IDYES) {
                skipped++;
                continue;
            }
        }
        /* the space, as the game's copy checks it (T074: the size and 2 KB
         * more free), less the save it replaces */
        const int freeKB = save_card::kCardKB - save_card::used_kb(folder_path(target)) +
                           (there ? save_card::used_kb(to) : 0);
        if (s.kb + 2 > freeKB) {
            problem = trf(L"%s has not enough space for %s: it needs %s KB, %s KB free.", target.c_str(),
                          save_card::title(s).c_str(), save_card::number(s.kb).c_str(),
                          save_card::number(std::max(0, freeKB)).c_str());
            skipped++;
            continue;
        }
        std::wstring error;
        if (there && !save_card::remove_tree(to, true, &error)) {
            problem = trf(L"Could not replace %s in %s: %s", s.dir.c_str(), target.c_str(), error.c_str());
            skipped++;
            continue;
        }
        if (!save_card::copy_tree(from, to, &error)) {
            save_card::remove_tree(to, true, nullptr);      /* (no half save left) */
            problem = trf(L"Could not copy %s: %s", s.dir.c_str(), error.c_str());
            skipped++;
            continue;
        }
        copied++;
    }
    refresh();
    std::wstring t = copied == 1 ? trf(L"Copied 1 save to %s.", target.c_str())
                                 : trf(L"Copied %s saves to %s.", save_card::number(copied).c_str(), target.c_str());
    if (skipped > 0)
        t += L" " + trf(L"%s not copied.", save_card::number(skipped).c_str());
    if (!problem.empty()) {
        t += L" " + problem;
        MessageBoxW(g.wnd, problem.c_str(), tr(kTitle), MB_OK | MB_ICONWARNING);
    }
    set_status(t);
    notify();
}

void copy_menu()
{
    Quiet quiet;
    if (selected_saves().empty())
        return;
    HMENU menu = CreatePopupMenu();
    g.copyTargets.clear();
    for (const Folder &f : g.list) {
        if (same(f.name, g.current))
            continue;
        const std::wstring item =
            f.name + L"\t" + trf(L"%s KB free", save_card::number(std::max(0, save_card::kCardKB - f.kb)).c_str());
        AppendMenuW(menu, MF_STRING, IDC_COPY_TO0 + (UINT_PTR)g.copyTargets.size(), item.c_str());
        g.copyTargets.push_back(f.name);
    }
    if (g.copyTargets.empty()) {
        DestroyMenu(menu);
        set_status(tr(L"There is no other save folder to copy to: New makes one."));
        return;
    }
    RECT r;
    GetWindowRect(g.copy, &r);
    const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, r.left,
                                        r.bottom, 0, g.wnd, nullptr);
    DestroyMenu(menu);
    if (cmd >= IDC_COPY_TO0 && cmd < IDC_COPY_TO0 + (int)g.copyTargets.size())
        copy_saves(g.copyTargets[(size_t)(cmd - IDC_COPY_TO0)]);
}

void delete_saves()
{
    Quiet quiet;
    std::vector<save_card::Save> saves;
    for (int i : selected_saves())
        saves.push_back(g.card.saves[(size_t)i]);
    const std::wstring source = g.current;
    if (saves.empty())
        return;
    std::wstring q;
    if (saves.size() == 1) {
        const save_card::Save &s = saves[0];
        q = trf(L"Delete this save from %s?", source.c_str()) + L"\n\n" + save_card::title(s) + L"\n" +
            trf(L"%s, %s KB, saved %s", save_card::kind_name(s.kind).c_str(), save_card::number(s.kb).c_str(),
                save_card::date_text(s.saved).c_str());
    } else {
        int kb = 0;
        for (const save_card::Save &s : saves)
            kb += s.kb;
        q = trf(L"Delete these %d saves from %s?\n\n%s KB.", (int)saves.size(), source.c_str(),
                save_card::number(kb).c_str());
    }
    q += std::wstring(L"\n\n") + tr(L"This cannot be undone.");
    if (MessageBoxW(g.wnd, q.c_str(), tr(kTitle), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    int deleted = 0;
    std::wstring problem;
    for (const save_card::Save &s : saves) {
        std::wstring error;
        if (save_card::remove_tree(folder_path(source) + L"\\" + s.dir, true, &error))
            deleted++;
        else
            problem = error;
    }
    refresh();
    set_status((deleted == 1 ? std::wstring(tr(L"Deleted 1 save."))
                             : trf(L"Deleted %s saves.", save_card::number(deleted).c_str())) +
               (problem.empty() ? std::wstring() : L" " + trf(L"Not deleted: %s", problem.c_str())));
    notify();
}

/* ---- .psu files (psu.h): a PS2 save in one file; the game's cards are folders ---- */

/* One .psu written, or its problem ("" in *error: written). */
bool write_bytes(const std::wstring &path, const std::vector<uint8_t> &data, const FILETIME &when, std::wstring *error)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = util::system_error_text(GetLastError());
        return false;
    }
    DWORD put = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    if (!ok)
        *error = util::system_error_text(GetLastError());
    else if ((when.dwLowDateTime | when.dwHighDateTime) != 0)
        SetFileTime(h, nullptr, nullptr, &when);
    CloseHandle(h);
    return ok;
}

bool read_whole(const std::wstring &path, std::vector<uint8_t> *out, std::wstring *error)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = util::system_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= 0x2000000;   /* (32 MB: far over any save) */
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    if (!ok)
        *error = size.QuadPart > 0x2000000 ? std::wstring(tr(L"It is too big to be a save file.")) : util::system_error_text(GetLastError());
    CloseHandle(h);
    return ok;
}

/* The .psu files chosen in an Open dialog: full paths. */
std::vector<std::wstring> choose_psu_files()
{
    std::wstring buf(65536, L'\0');
    std::wstring filter = tr(L"PS2 save files");
    filter += L" (*.psu)";
    filter.push_back(L'\0');
    filter += L"*.psu";
    filter.push_back(L'\0');
    filter += tr(L"All files");
    filter += L" (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = tr(L"Import PS2 saves (.psu) into this folder");
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                OFN_ALLOWMULTISELECT;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn))
        return out;
    const wchar_t *p = buf.c_str();
    const std::wstring first = p;
    p += first.size() + 1;
    if (*p == 0) {                          /* one file: its whole path */
        out.push_back(first);
        return out;
    }
    for (; *p != 0; p += wcslen(p) + 1)     /* several: the folder, then the names */
        out.push_back(util::with_slash(first) + p);
    return out;
}

void import_psu()
{
    Quiet quiet;
    const std::wstring target = g.current;
    if (target.empty()) {
        set_status(tr(L"Select a save folder first (New makes one): the saves are imported into it."));
        return;
    }
    const std::vector<std::wstring> files = choose_psu_files();
    int imported = 0, skipped = 0;
    std::wstring problem;
    for (const std::wstring &path : files) {
        const wchar_t *base = wcsrchr(path.c_str(), L'\\');
        const std::wstring name = base != nullptr ? base + 1 : path;
        std::vector<uint8_t> bytes;
        psu::Save save;
        std::wstring error;
        if (!read_whole(path, &bytes, &error) || !psu::parse(bytes, &save, &error)) {
            problem = name + L": " + error;
            skipped++;
            continue;
        }
        const std::wstring dir = util::widen(save.dir), to = folder_path(target) + L"\\" + dir;
        const bool there = GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (there) {
            const int a = MessageBoxW(g.wnd,
                                      trf(L"%s has this save already:\n%s\n\nReplace it with the one in %s?", target.c_str(),
                                          dir.c_str(), name.c_str())
                                          .c_str(),
                                      tr(kTitle), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (a == IDCANCEL)
                break;
            if (a != IDYES) {
                skipped++;
                continue;
            }
        }
        /* the space as the game's copy checks it: the clusters and two more, two KB free */
        int kb = 2;
        for (const psu::File &f : save.files)
            kb += std::max(1, (int)((f.data.size() + 1023) / 1024));
        const int freeKB = save_card::kCardKB - save_card::used_kb(folder_path(target)) +
                           (there ? save_card::used_kb(to) : 0);
        if (kb + 2 > freeKB) {
            problem = trf(L"%s has not enough space for %s: it needs %s KB, %s KB free.", target.c_str(), dir.c_str(),
                          save_card::number(kb).c_str(), save_card::number(std::max(0, freeKB)).c_str());
            skipped++;
            continue;
        }
        if (there && !save_card::remove_tree(to, true, &error)) {
            problem = trf(L"Could not replace %s in %s: %s", dir.c_str(), target.c_str(), error.c_str());
            skipped++;
            continue;
        }
        bool ok = util::make_folders(to);
        if (!ok)
            error = util::system_error_text(GetLastError());
        for (size_t i = 0; ok && i < save.files.size(); i++)
            ok = write_bytes(to + L"\\" + util::widen(save.files[i].name), save.files[i].data, save.files[i].modified, &error);
        if (!ok) {
            save_card::remove_tree(to, true, nullptr);      /* (no half save left) */
            problem = trf(L"Could not import %s: %s", dir.c_str(), error.c_str());
            skipped++;
            continue;
        }
        imported++;
    }
    refresh(target);
    if (files.empty())
        return;
    std::wstring t = imported == 1 ? trf(L"Imported 1 save into %s.", target.c_str())
                                   : trf(L"Imported %s saves into %s.", save_card::number(imported).c_str(), target.c_str());
    if (skipped > 0)
        t += L" " + trf(L"%s not imported.", save_card::number(skipped).c_str());
    if (!problem.empty()) {
        t += L" " + problem;
        MessageBoxW(g.wnd, problem.c_str(), tr(kTitle), MB_OK | MB_ICONWARNING);
    }
    set_status(t);
    notify();
}

void export_psu()
{
    Quiet quiet;
    std::vector<save_card::Save> saves;
    for (int i : selected_saves())
        saves.push_back(g.card.saves[(size_t)i]);
    if (saves.empty()) {
        set_status(tr(L"Select the saves to export first."));
        return;
    }
    const std::wstring dest = util::choose_folder(g.wnd, tr(L"Choose the folder for the .psu files"), tr(L"Export"));
    if (dest.empty())
        return;
    int exported = 0, skipped = 0;
    std::wstring problem;
    for (const save_card::Save &s : saves) {
        const std::wstring from = folder_path(g.current) + L"\\" + s.dir, out = util::with_slash(dest) + s.dir + L".psu";
        psu::Save save;
        save.dir = util::narrow(s.dir);
        std::wstring error;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
        bool ok = h != INVALID_HANDLE_VALUE;
        for (bool more = ok; more; more = FindNextFileW(h, &fd) != 0) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
            psu::File f;
            f.name = util::narrow(fd.cFileName);
            f.modified = fd.ftLastWriteTime;
            if (!read_whole(from + L"\\" + fd.cFileName, &f.data, &error)) {
                ok = false;
                break;
            }
            save.files.push_back(std::move(f));
        }
        if (h != INVALID_HANDLE_VALUE)
            FindClose(h);
        if (ok && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const int a = MessageBoxW(g.wnd, trf(L"%s exists already.\n\nReplace it?", out.c_str()).c_str(), tr(kTitle),
                                      MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (a == IDCANCEL)
                break;
            if (a != IDYES) {
                skipped++;
                continue;
            }
        }
        FILETIME none = {};
        if (ok)
            ok = write_bytes(out, psu::build(save), none, &error);
        if (!ok) {
            problem = trf(L"Could not export %s: %s", s.dir.c_str(), error.c_str());
            skipped++;
            continue;
        }
        exported++;
    }
    std::wstring t = exported == 1 ? trf(L"Exported 1 save to %s.", dest.c_str())
                                   : trf(L"Exported %s saves to %s.", save_card::number(exported).c_str(), dest.c_str());
    if (skipped > 0)
        t += L" " + trf(L"%s not exported.", save_card::number(skipped).c_str());
    if (!problem.empty()) {
        t += L" " + problem;
        MessageBoxW(g.wnd, problem.c_str(), tr(kTitle), MB_OK | MB_ICONWARNING);
    }
    set_status(t);
}

/* ---- the window ---- */

void make_font()
{
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT f = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi))
        f = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND c = GetWindow(g.wnd, GW_CHILD); c != nullptr; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    if (g.font != nullptr)
        DeleteObject(g.font);
    g.font = f;
}

/* PC (2026-11-16): a control's text's width / wrapped height in its font
 * (the interface's languages run longer: layout sizes the slot labels, the
 * buttons and the hint from them) */
SIZE text_size(HWND c, int wrapWidth = 0)
{
    wchar_t t[1024] = L"";
    GetWindowTextW(c, t, 1024);
    HDC dc = GetDC(c);
    HFONT f = (HFONT)SendMessageW(c, WM_GETFONT, 0, 0);
    HGDIOBJ old = SelectObject(dc, f != nullptr ? (HGDIOBJ)f : GetStockObject(DEFAULT_GUI_FONT));
    RECT r = {0, 0, wrapWidth, 0};
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | (wrapWidth > 0 ? DT_WORDBREAK : DT_SINGLELINE));
    SelectObject(dc, old);
    ReleaseDC(c, dc);
    return {r.right, r.bottom};
}

void layout()
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int W = rc.right, H = rc.bottom, m = px(kM), gap = px(kGap), bh = px(kBtnH);
    auto put = [](HWND c, int x, int y, int w, int h) { SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); };
    /* (PC 2026-11-16: the hint as tall as its text needs, two lines at least) */
    const int hintH = std::max(px(kHintH), (int)text_size(g.hint, W - 2 * m).cy + px(2));
    put(g.hint, m, m, W - 2 * m, hintH);
    put(g.path, m, m + hintH, W - 2 * m, px(kPathH));
    int y = m + hintH + px(kPathH) + gap, x = m;
    for (int s = 0; s < save_folders::kSlots; s++) {
        /* (PC 2026-11-16: the label as wide as its text, kSlotLabelW at least) */
        const int sw = std::max(px(kSlotLabelW), (int)text_size(g.slotLabel[s]).cx + px(8));
        put(g.slotLabel[s], x, y + px(5), sw, px(kLabelH));
        put(g.slot[s], x + sw, y, px(kSlotComboW), px(300));
        x += sw + px(kSlotComboW) + px(24);
    }
    const int top = y + bh + px(12), bottom = H - m - bh, row = bottom - gap - bh;
    /* left: the folders */
    const int lw = px(kLeftW);
    put(g.foldersLabel, m, top, lw, px(kLabelH));
    put(g.folders, m, top + px(kLabelH) + px(2), lw, row - gap - (top + px(kLabelH) + px(2)));
    /* (PC 2026-11-16: the four buttons share the row by their texts' widths,
     * the room left over alike; equal quarters before) */
    HWND fb[4] = {g.newFolder, g.rename, g.clear, g.remove};
    int need[4], sum = 0;
    for (int i = 0; i < 4; i++) {
        need[i] = (int)text_size(fb[i]).cx + px(14);
        sum += need[i];
    }
    const int room = lw - 3 * px(6);
    int bx = m;
    for (int i = 0; i < 4; i++) {
        const int bw = sum <= room ? need[i] + (room - sum) / 4 : need[i] * room / sum;
        put(fb[i], bx, row, bw, bh);
        bx += bw + px(6);
    }
    /* right: the statistics, the saves, the details */
    const int rx = m + lw + px(16), rw = W - m - rx;
    put(g.stats, rx, top, rw, px(kStatsH));
    const int listTop = top + px(kStatsH) + px(4), detailsTop = row - gap - px(kDetailsH);
    put(g.contents, rx, listTop, rw, detailsTop - gap - listTop);
    put(g.details, rx, detailsTop, rw, px(kDetailsH));
    /* (the four buttons by their texts' widths; squeezed together when the room is short) */
    HWND sb[4] = {g.copy, g.removeSaves, g.importPsu, g.exportPsu};
    int sw[4], ssum = 0;
    for (int i = 0; i < 4; i++) {
        sw[i] = std::max(px(100), (int)text_size(sb[i]).cx + px(16));
        ssum += sw[i];
    }
    const int sroom = rw - 3 * px(6);
    int sx = rx;
    for (int i = 0; i < 4; i++) {
        const int bw = ssum <= sroom ? sw[i] : sw[i] * sroom / ssum;
        put(sb[i], sx, row, bw, bh);
        sx += bw + px(6);
    }
    /* the bottom */
    put(g.status, m, bottom + px(6), W - 2 * m - px(kCloseW) - gap, px(20));
    put(g.close, W - m - px(kCloseW), bottom, px(kCloseW), bh);
    folder_columns();
    content_columns();
}

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(g.wnd, GWLP_HINSTANCE), nullptr);
}

void create_controls()
{
    g.hint = make(WC_STATICW,
                  tr(L"Each save folder is one memory card (8 MB). The game's memory card slots take the folders "
                     L"picked here when it starts; in the game, F2 > MEMORY CARDS swaps them."),
                  SS_LEFT | SS_NOPREFIX, IDC_HINT);
    g.path = make(WC_STATICW, trf(L"The folders: %s", g.dir.c_str()).c_str(), SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS,
                  IDC_PATH);
    g.slotLabel[0] = make(WC_STATICW, tr(L"Memory card slot &1:"), SS_LEFT, IDC_SLOT_LABEL1);
    g.slot[0] = make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_SLOT1);
    g.slotLabel[1] = make(WC_STATICW, tr(L"Memory card slot &2:"), SS_LEFT, IDC_SLOT_LABEL2);
    g.slot[1] = make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_SLOT2);
    g.foldersLabel = make(WC_STATICW, tr(L"Save &folders"), SS_LEFT, IDC_FOLDERS_LABEL);
    g.folders = make(WC_LISTVIEWW, L"",
                     WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_EDITLABELS,
                     IDC_FOLDERS, WS_EX_CLIENTEDGE);
    SendMessageW(g.folders, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    g.newFolder = make(WC_BUTTONW, tr(L"&New"), WS_TABSTOP | BS_PUSHBUTTON, IDC_NEW);
    g.rename = make(WC_BUTTONW, tr(L"&Rename"), WS_TABSTOP | BS_PUSHBUTTON, IDC_RENAME);
    g.clear = make(WC_BUTTONW, tr(L"C&lear..."), WS_TABSTOP | BS_PUSHBUTTON, IDC_CLEAR);
    g.remove = make(WC_BUTTONW, tr(L"&Delete..."), WS_TABSTOP | BS_PUSHBUTTON, IDC_DELETE);
    g.stats = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, IDC_STATS);
    g.contents = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, IDC_CONTENTS,
                      WS_EX_CLIENTEDGE);
    SendMessageW(g.contents, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    g.details = make(WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_DETAILS,
                     WS_EX_CLIENTEDGE);
    g.copy = make(WC_BUTTONW, tr(L"&Copy to..."), WS_TABSTOP | BS_PUSHBUTTON, IDC_COPY);
    g.removeSaves = make(WC_BUTTONW, tr(L"Delete sa&ves..."), WS_TABSTOP | BS_PUSHBUTTON, IDC_DELETE_SAVES);
    g.importPsu = make(WC_BUTTONW, tr(L"&Import .psu..."), WS_TABSTOP | BS_PUSHBUTTON, IDC_IMPORT);
    g.exportPsu = make(WC_BUTTONW, tr(L"E&xport .psu..."), WS_TABSTOP | BS_PUSHBUTTON, IDC_EXPORT);
    g.status = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS);
    g.close = make(WC_BUTTONW, tr(L"Close"), WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY: {
        const NMHDR *n = reinterpret_cast<const NMHDR *>(lp);
        if (n->hwndFrom == g.folders) {
            if (n->code == LVN_ITEMCHANGED) {
                /* (one show_card once the list is done: a click deselects
                 * one row, then selects another) */
                const NMLISTVIEW *l = reinterpret_cast<const NMLISTVIEW *>(lp);
                if ((l->uChanged & LVIF_STATE) && !g.filling && ((l->uNewState ^ l->uOldState) & LVIS_SELECTED) &&
                    !g.showPosted) {
                    g.showPosted = true;
                    PostMessageW(h, WM_APP_SHOW, 0, 0);
                }
            } else if (n->code == LVN_BEGINLABELEDITW) {
                HWND e = ListView_GetEditControl(g.folders);
                if (e != nullptr)
                    SendMessageW(e, EM_LIMITTEXT, save_folders::kMaxName, 0);
                return FALSE;
            } else if (n->code == LVN_ENDLABELEDITW) {
                const NMLVDISPINFOW *d = reinterpret_cast<const NMLVDISPINFOW *>(lp);
                return end_rename(d->item.iItem, d->item.pszText) ? TRUE : FALSE;
            } else if (n->code == LVN_KEYDOWN) {
                const WORD key = reinterpret_cast<const NMLVKEYDOWN *>(lp)->wVKey;
                if (key == VK_F2)
                    begin_rename();
                else if (key == VK_DELETE)
                    delete_folder();
            }
        } else if (n->hwndFrom == g.contents) {
            if (n->code == LVN_ITEMCHANGED) {
                const NMLISTVIEW *l = reinterpret_cast<const NMLISTVIEW *>(lp);
                if ((l->uChanged & LVIF_STATE) && !g.filling && ((l->uNewState ^ l->uOldState) & LVIS_SELECTED))
                    show_details();
            } else if (n->code == LVN_KEYDOWN) {
                if (reinterpret_cast<const NMLVKEYDOWN *>(lp)->wVKey == VK_DELETE)
                    delete_saves();
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SLOT1:
        case IDC_SLOT2:
            if (HIWORD(wp) == CBN_SELCHANGE && !g.filling)
                slot_changed(LOWORD(wp) == IDC_SLOT1 ? 0 : 1);
            return 0;
        case IDC_NEW:
            new_folder();
            return 0;
        case IDC_RENAME:
            begin_rename();
            return 0;
        case IDC_CLEAR:
            clear_folder();
            return 0;
        case IDC_DELETE:
            delete_folder();
            return 0;
        case IDC_COPY:
            copy_menu();
            return 0;
        case IDC_DELETE_SAVES:
            delete_saves();
            return 0;
        case IDC_IMPORT:
            import_psu();
            return 0;
        case IDC_EXPORT:
            export_psu();
            return 0;
        case IDCANCEL:                      /* Close, Esc */
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_APP_RENAMED:                    /* a rename taken by the list: everything read again */
        refresh(g.current);
        notify();
        return 0;
    case WM_APP_SHOW: {
        g.showPosted = false;
        const int f = selected_folder();
        if (!same(f >= 0 ? g.list[(size_t)f].name : std::wstring(), g.current))
            show_card();
        else
            buttons();
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        make_font();
        layout();
        show_card();                        /* (the icons at the new size) */
        return 0;
    }
    case WM_ACTIVATE:
        /* the folders as they are now (the game may have saved, Explorer
         * changed them) - not under an open name edit */
        if (LOWORD(wp) != WA_INACTIVE && !editing() && g.quiet == 0)
            refresh();
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.details) {        /* (read-only sends this: a box like the lists) */
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        /* (the saves' image list is the list view's: destroyed with it) */
        if (g.font != nullptr)
            DeleteObject(g.font);
        g = Manager();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

std::wstring summary(const std::wstring &savesDir, const std::wstring &engineIni)
{
    std::wstring t;
    for (int s = 0; s < save_folders::kSlots; s++) {
        const std::wstring f = save_folders::slot(engineIni, s);
        const DWORD a = f.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW((savesDir + f).c_str());
        const bool there = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        t += (s > 0 ? L"\n" : L"") + (f.empty() ? trf(L"Slot %d: no card", s + 1)
                                      : there   ? trf(L"Slot %d: %s", s + 1, f.c_str())
                                                : trf(L"Slot %d: %s (not found)", s + 1, f.c_str()));
    }
    return t;
}

void open(HWND owner, const std::wstring &savesDir, const std::wstring &engineIni, const std::wstring &engine,
          void (*changed)())
{
    if (g.wnd != nullptr) {
        ShowWindow(g.wnd, SW_RESTORE);
        SetForegroundWindow(g.wnd);
        return;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = (HICON)SendMessageW(owner, WM_GETICON, ICON_BIG, 0);
        wc.hIconSm = (HICON)SendMessageW(owner, WM_GETICON, ICON_SMALL, 0);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g = Manager();
    g.owner = owner;
    g.dir = util::with_slash(savesDir);
    g.ini = engineIni;
    g.engine = engine;
    g.changed = changed;
    g.dpi = GetDpiForWindow(owner);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, ex = WS_EX_CONTROLPARENT;
    RECT r = {0, 0, px(kW), px(kH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g.dpi);
    RECT o = {};
    GetWindowRect(owner, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const std::wstring title = std::wstring(tr(kTitle)) + L" - " + engine;
    g.wnd = CreateWindowExW(ex, kClass, title.c_str(), style, o.left + ((o.right - o.left) - ww) / 2,
                            o.top + ((o.bottom - o.top) - wh) / 2, ww, wh, owner, nullptr, inst, nullptr);
    if (g.wnd == nullptr)
        return;
    create_controls();
    make_font();
    layout();
    refresh();
    set_status(summary_line());
    ShowWindow(g.wnd, SW_SHOW);
    SetFocus(g.list.empty() ? g.newFolder : g.folders);
}

bool is_open()
{
    return g.wnd != nullptr;
}

bool filter(MSG &m)
{
    /* (not while a folder's name is edited: its Enter and Esc are the edit's) */
    return g.wnd != nullptr && !editing() && (m.hwnd == g.wnd || IsChild(g.wnd, m.hwnd)) &&
           IsDialogMessageW(g.wnd, &m);
}

} // namespace save_manager
