/* psu.h - the .psu save file (EMS Adapter / PSU format: what PCSX2, mymc and
 * most save sites use for ONE PS2 save), read and written for the save
 * manager's Import / Export (save_manager.h).  PC (2026-11-24).
 *
 * A .psu is a list of 512-byte entries: the save's directory (its size is the
 * number of entries it holds, "." and ".." included), "." and "..", then each
 * file as an entry followed by its data padded to 1024 bytes.  An entry: mode
 * (u16; 0x8427 a directory, 0x8497 a file), size (u32 at 4), created (8 bytes at
 * 8) and modified (8 bytes at 0x18) as [0, sec, min, hour, day, month, year u16],
 * the name at 0x40 (up to 448 bytes, ends with 0).
 *
 * The game's memory cards are folders (platform/memcard.h): a save is the
 * folder BASCUS-97102GAMEDATA\ holding icon.sys, icon0.ico, thumbnail and the
 * data file; a .psu is those files in one, which the game cannot read as it
 * is - import unpacks it, export packs one. */
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace psu {

struct File {
    std::string name;                   /* a plain file name: no folders, no ':' */
    std::vector<uint8_t> data;
    FILETIME modified = {};             /* when it was written (UTC; 0: not known) */
};

struct Save {
    std::string dir;                    /* the save's folder name (BASCUS-97102GAMEDATA) */
    std::vector<File> files;
};

/* A .psu's bytes read: false with *error (a sentence) when it is not one or
 * is damaged.  Names that could leave the save's folder (a path, "..") are
 * refused: an import must never write outside its folder. */
bool parse(const std::vector<uint8_t> &bytes, Save *out, std::wstring *error);

/* A save's .psu bytes. */
std::vector<uint8_t> build(const Save &save);

/* A name that is safe as a file or folder name of a card. */
bool plain_name(const std::string &n);

} // namespace psu
