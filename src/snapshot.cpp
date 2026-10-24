// Copyright (c) 2026 The NetCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "snapshot.h"

#include "ui_interface.h"
#include "util.h"

#include "miniz/miniz.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

namespace fs = boost::filesystem;

namespace {

/** Snapshots published with this version (height, best block hash). */
struct KnownSnapshot { int nHeight; const char* pszHash; };
const KnownSnapshot vKnownSnapshots[] = {
    { 3357862, "e356e35cce22aee9dbb2b0041c9c93227e04bb9b376621079e5c35014d226533" },
};

const char* MANIFEST = "snapshot.txt";

FILE* OpenFile(const fs::path& p)
{
#ifdef WIN32
    return _wfopen(p.wstring().c_str(), L"rb");
#else
    return fopen(p.string().c_str(), "rb");
#endif
}

/** Owns an opened archive. */
class ZipReader
{
public:
    mz_zip_archive zip;
    FILE* file;
    bool fOpen;

    ZipReader() : file(NULL), fOpen(false) { memset(&zip, 0, sizeof(zip)); }
    ~ZipReader()
    {
        if (fOpen) mz_zip_reader_end(&zip);
        if (file) fclose(file);
    }
    bool Open(const fs::path& p, std::string& strError)
    {
        boost::system::error_code ec;
        uint64_t nSize = fs::file_size(p, ec);
        file = ec ? NULL : OpenFile(p);
        if (!file) {
            strError = strprintf(_("Cannot open %s"), p.string().c_str());
            return false;
        }
        if (!mz_zip_reader_init_cfile(&zip, file, nSize, 0)) {
            strError = strprintf(_("%s is not a valid snapshot archive (%s)"), p.string().c_str(),
                                 mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
            return false;
        }
        fOpen = true;
        return true;
    }
};

/** Files a snapshot may contain: blk0001.dat .. blk9999.dat and txleveldb/<file>. */
bool AllowedEntry(const std::string& strName, bool& fSkip)
{
    fSkip = false;
    if (strName == MANIFEST)
        return true;
    if (strName.size() == 11 && strName.compare(0, 3, "blk") == 0 && strName.compare(7, 4, ".dat") == 0) {
        for (int i = 3; i < 7; i++)
            if (strName[i] < '0' || strName[i] > '9')
                return false;
        return true;
    }
    if (strName == "txleveldb/") {
        fSkip = true;
        return true;
    }
    if (strName.compare(0, 10, "txleveldb/") == 0) {
        std::string strFile = strName.substr(10);
        if (strFile.empty() || strFile.find('/') != std::string::npos || strFile.find('\\') != std::string::npos ||
            strFile == "." || strFile == "..")
            return false;
        // files of a running database
        if (strFile == "LOCK" || strFile == "LOG" || strFile == "LOG.old")
            fSkip = true;
        return true;
    }
    return false;
}

bool ParseManifest(const std::string& strText, SnapshotInfo& info)
{
    std::vector<std::string> vLines;
    std::string strLine;
    for (size_t i = 0; i <= strText.size(); i++) {
        if (i == strText.size() || strText[i] == '\n') {
            if (!strLine.empty() && strLine[strLine.size() - 1] == '\r')
                strLine.erase(strLine.size() - 1);
            vLines.push_back(strLine);
            strLine.clear();
        } else {
            strLine += strText[i];
        }
    }
    if (vLines.empty() || vLines[0] != "netcoin-snapshot 1")
        return false;
    bool fHeight = false, fHash = false;
    for (size_t i = 1; i < vLines.size(); i++) {
        size_t eq = vLines[i].find('=');
        if (eq == std::string::npos)
            continue;
        std::string strKey = vLines[i].substr(0, eq), strValue = vLines[i].substr(eq + 1);
        if (strKey == "height") {
            info.nHeight = atoi(strValue.c_str());
            fHeight = info.nHeight >= 0 && !strValue.empty();
        } else if (strKey == "hash") {
            fHash = strValue.size() == 64 && IsHex(strValue);
            info.hash.SetHex(strValue);
        } else if (strKey == "created") {
            info.strCreated = strValue;
        }
    }
    return fHeight && fHash;
}

bool ReadInfo(ZipReader& reader, SnapshotInfo& info, std::string& strError)
{
    mz_uint nFiles = mz_zip_reader_get_num_files(&reader.zip);
    bool fManifest = false, fIndex = false, fBlocks = false;
    info.nDataSize = 0;
    for (mz_uint i = 0; i < nFiles; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&reader.zip, i, &st)) {
            strError = _("The snapshot archive is damaged");
            return false;
        }
        std::string strName = st.m_filename;
        bool fSkip;
        if (!AllowedEntry(strName, fSkip)) {
            strError = strprintf(_("Unexpected file in the snapshot: %s"), strName.c_str());
            return false;
        }
        if (strName == MANIFEST) {
            if (st.m_uncomp_size > 4096) {
                strError = _("The snapshot archive is damaged");
                return false;
            }
            std::vector<char> vText(st.m_uncomp_size + 1, 0);
            if (!mz_zip_reader_extract_to_mem(&reader.zip, i, &vText[0], st.m_uncomp_size, 0) ||
                !ParseManifest(std::string(&vText[0], st.m_uncomp_size), info)) {
                strError = _("The snapshot description (snapshot.txt) is missing or invalid");
                return false;
            }
            fManifest = true;
        } else if (!fSkip) {
            info.nDataSize += st.m_uncomp_size;
            if (strName.compare(0, 3, "blk") == 0)
                fBlocks = true;
            else
                fIndex = true;
        }
    }
    if (!fManifest || !fBlocks || !fIndex) {
        strError = _("The archive is not a NetCoin block chain snapshot (snapshot.txt, blk0001.dat or txleveldb missing)");
        return false;
    }
    info.fKnown = IsKnownSnapshot(info.nHeight, info.hash);
    return true;
}

struct ExtractState
{
    fs::ofstream* pOut;
    uint64_t nDone;
    uint64_t nTotal;
    int nLastPercent;
};

size_t WriteCallback(void* pOpaque, mz_uint64 /*nOfs*/, const void* pBuf, size_t n)
{
    ExtractState* state = (ExtractState*)pOpaque;
    state->pOut->write((const char*)pBuf, n);
    if (!state->pOut->good())
        return 0;
    state->nDone += n;
    int nPercent = state->nTotal ? (int)(state->nDone * 100 / state->nTotal) : 100;
    if (nPercent != state->nLastPercent) {
        state->nLastPercent = nPercent;
        uiInterface.InitMessage(strprintf(_("Importing block chain snapshot... %d%%"), nPercent));
    }
    return n;
}

} // namespace

bool IsKnownSnapshot(int nHeight, const uint256& hash)
{
    for (size_t i = 0; i < sizeof(vKnownSnapshots) / sizeof(vKnownSnapshots[0]); i++)
        if (vKnownSnapshots[i].nHeight == nHeight && uint256(vKnownSnapshots[i].pszHash) == hash)
            return true;
    return false;
}

bool ReadSnapshotInfo(const fs::path& file, SnapshotInfo& info, std::string& strError)
{
    ZipReader reader;
    return reader.Open(file, strError) && ReadInfo(reader, info, strError);
}

bool ImportSnapshot(const fs::path& file, SnapshotInfo& info, fs::path& pathBackup, std::string& strError)
{
    ZipReader reader;
    if (!reader.Open(file, strError) || !ReadInfo(reader, info, strError))
        return false;

    const fs::path dataDir = GetDataDir();
    const fs::path tmpDir = dataDir / "snapshot.tmp";
    boost::system::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir / "txleveldb", ec);
    if (ec) {
        strError = strprintf(_("Cannot create %s"), tmpDir.string().c_str());
        return false;
    }

    // enough space for the extracted files?
    fs::space_info space = fs::space(dataDir, ec);
    if (!ec && space.available < info.nDataSize + 100 * 1024 * 1024) {
        strError = strprintf(_("Not enough disk space: the snapshot needs %d MB"), (int)(info.nDataSize / 1048576) + 100);
        fs::remove_all(tmpDir, ec);
        return false;
    }

    printf("Importing snapshot %s (height %d, %s)\n", file.string().c_str(), info.nHeight, info.hash.ToString().c_str());
    ExtractState state;
    state.nDone = 0;
    state.nTotal = info.nDataSize;
    state.nLastPercent = -1;
    mz_uint nFiles = mz_zip_reader_get_num_files(&reader.zip);
    for (mz_uint i = 0; i < nFiles; i++) {
        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&reader.zip, i, &st);
        std::string strName = st.m_filename;
        bool fSkip;
        if (!AllowedEntry(strName, fSkip) || fSkip || strName == MANIFEST)
            continue;
        fs::path dest = tmpDir;
        if (strName.compare(0, 10, "txleveldb/") == 0)
            dest = dest / "txleveldb" / strName.substr(10);
        else
            dest = dest / strName;
        fs::ofstream out(dest, std::ios::binary | std::ios::trunc);
        state.pOut = &out;
        bool fOk = out.good() && mz_zip_reader_extract_to_callback(&reader.zip, i, WriteCallback, &state, 0);
        out.close();
        if (!fOk || out.fail()) {
            strError = strprintf(_("Extracting %s from the snapshot failed (%s)"), strName.c_str(),
                                 mz_zip_get_error_string(mz_zip_get_last_error(&reader.zip)));
            fs::remove_all(tmpDir, ec);
            return false;
        }
    }

    // move the current block chain out of the way
    std::vector<fs::path> vOld;
    for (fs::directory_iterator it(dataDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string strName = it->path().filename().string();
        bool fSkip;
        if (strName == "txleveldb" || (strName != MANIFEST && AllowedEntry(strName, fSkip) && strName.compare(0, 3, "blk") == 0))
            vOld.push_back(it->path());
    }
    pathBackup = fs::path();
    if (!vOld.empty()) {
        pathBackup = dataDir / ("chain-backup-" + DateTimeStrFormat("%Y%m%d-%H%M%S", GetTime()));
        fs::create_directories(pathBackup, ec);
        for (size_t i = 0; i < vOld.size() && !ec; i++)
            fs::rename(vOld[i], pathBackup / vOld[i].filename(), ec);
        if (ec) {
            strError = strprintf(_("Cannot move the old block chain to %s: %s"), pathBackup.string().c_str(), ec.message().c_str());
            return false;
        }
    }

    // and put the snapshot in its place
    std::vector<fs::path> vNew;
    for (fs::directory_iterator it(tmpDir, ec), end; !ec && it != end; it.increment(ec))
        vNew.push_back(it->path());
    for (size_t i = 0; i < vNew.size(); i++) {
        const fs::path& p = vNew[i];
        fs::rename(p, dataDir / p.filename(), ec);
        if (ec) {
            strError = strprintf(_("Cannot move %s into the data directory: %s"), p.filename().string().c_str(), ec.message().c_str());
            return false;
        }
    }
    fs::remove_all(tmpDir, ec);
    printf("Snapshot imported%s%s\n", pathBackup.empty() ? "" : ", old block chain moved to ", pathBackup.string().c_str());
    return true;
}
