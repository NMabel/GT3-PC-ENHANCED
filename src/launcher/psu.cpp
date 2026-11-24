/* psu.cpp - the .psu save file (psu.h). */
#include "psu.h"

#include "platform/ui_text.h"

#include <algorithm>
#include <cstring>

namespace psu {
namespace {

constexpr size_t kEntry = 512, kNameAt = 0x40, kNameMax = 448;
constexpr uint16_t kDirMode = 0x8427, kFileMode = 0x8497;

uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

/* [0, sec, min, hour, day, month, year u16] (the PS2's own clock: shown as written) -> a FILETIME */
FILETIME stamp_to_filetime(const uint8_t *s)
{
    FILETIME out = {};
    SYSTEMTIME st = {};
    st.wYear = rd16(s + 6);
    st.wMonth = s[5];
    st.wDay = s[4];
    st.wHour = s[3];
    st.wMinute = s[2];
    st.wSecond = s[1];
    FILETIME local;
    if (st.wYear >= 1980 && st.wMonth >= 1 && st.wMonth <= 12 && st.wDay >= 1 && st.wDay <= 31 && st.wHour < 24 &&
        st.wMinute < 60 && st.wSecond < 60 && SystemTimeToFileTime(&st, &local))
        LocalFileTimeToFileTime(&local, &out);
    return out;
}

void filetime_to_stamp(const FILETIME &ft, uint8_t *s)
{
    memset(s, 0, 8);
    FILETIME local;
    SYSTEMTIME st;
    if ((ft.dwLowDateTime | ft.dwHighDateTime) == 0 || !FileTimeToLocalFileTime(&ft, &local) ||
        !FileTimeToSystemTime(&local, &st))
        return;
    s[1] = (uint8_t)st.wSecond;
    s[2] = (uint8_t)st.wMinute;
    s[3] = (uint8_t)st.wHour;
    s[4] = (uint8_t)st.wDay;
    s[5] = (uint8_t)st.wMonth;
    wr16(s + 6, st.wYear);
}

std::string entry_name(const uint8_t *e)
{
    size_t n = 0;
    while (n < kNameMax && e[kNameAt + n] != 0)
        n++;
    return std::string((const char *)e + kNameAt, n);
}

} // namespace

bool plain_name(const std::string &n)
{
    if (n.empty() || n.size() > 255 || n == "." || n == "..")
        return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|')
            return false;
    return n.back() != ' ' && n.back() != '.';
}

bool parse(const std::vector<uint8_t> &b, Save *out, std::wstring *error)
{
    auto fail = [&](const wchar_t *why) {
        if (error != nullptr)
            *error = tr(why);
        return false;
    };
    if (b.size() < 3 * kEntry)
        return fail(N_(L"This is not a PS2 save file (.psu): it is too small."));
    const uint8_t *head = b.data();
    if (rd16(head) != kDirMode)
        return fail(N_(L"This is not a PS2 save file (.psu): it does not start with a save folder."));
    Save s;
    s.dir = entry_name(head);
    if (!plain_name(s.dir))
        return fail(N_(L"This .psu has a save folder name that cannot be used."));
    const uint32_t count = rd32(head + 4);
    if (count < 2 || count > 512)
        return fail(N_(L"This .psu is damaged: its file count is not right."));
    size_t at = kEntry;
    for (uint32_t i = 0; i < count; i++) {
        if (at + kEntry > b.size())
            return fail(N_(L"This .psu is damaged: it ends before its last file."));
        const uint8_t *e = b.data() + at;
        at += kEntry;
        const uint16_t mode = rd16(e);
        const std::string name = entry_name(e);
        if (mode == kDirMode) {                 /* "." and ".." */
            if (name != "." && name != "..")
                return fail(N_(L"This .psu is damaged: it has a folder inside the save."));
            continue;
        }
        if ((mode & 0x0010) == 0)
            return fail(N_(L"This .psu is damaged: an entry is neither a file nor a folder."));
        const uint32_t size = rd32(e + 4);
        const size_t padded = ((size_t)size + 1023) / 1024 * 1024;
        if (size > 0x1000000 || at + padded > b.size())
            return fail(N_(L"This .psu is damaged: a file is cut short."));
        if (!plain_name(name))
            return fail(N_(L"This .psu has a file name that cannot be used."));
        File f;
        f.name = name;
        f.data.assign(b.begin() + (ptrdiff_t)at, b.begin() + (ptrdiff_t)(at + size));
        f.modified = stamp_to_filetime(e + 0x18);
        s.files.push_back(std::move(f));
        at += padded;
    }
    if (s.files.empty())
        return fail(N_(L"This .psu has no files in it."));
    *out = std::move(s);
    return true;
}

std::vector<uint8_t> build(const Save &s)
{
    std::vector<uint8_t> out;
    auto entry = [&](uint16_t mode, uint32_t size, const std::string &name, const FILETIME &when) {
        const size_t at = out.size();
        out.resize(at + kEntry, 0);
        uint8_t *e = out.data() + at;
        wr16(e, mode);
        wr32(e + 4, size);
        filetime_to_stamp(when, e + 8);
        filetime_to_stamp(when, e + 0x18);
        memcpy(e + kNameAt, name.data(), (std::min)(name.size(), kNameMax - 1));
    };
    FILETIME newest = {};
    for (const File &f : s.files)
        if (CompareFileTime(&f.modified, &newest) > 0)
            newest = f.modified;
    entry(kDirMode, (uint32_t)s.files.size() + 2, s.dir, newest);
    entry(kDirMode, 0, ".", newest);
    entry(kDirMode, 0, "..", newest);
    for (const File &f : s.files) {
        entry(kFileMode, (uint32_t)f.data.size(), f.name, f.modified);
        out.insert(out.end(), f.data.begin(), f.data.end());
        out.resize(out.size() + ((f.data.size() + 1023) / 1024 * 1024 - f.data.size()), 0);   /* (each file's own data padded to 1024) */
    }
    return out;
}

} // namespace psu
