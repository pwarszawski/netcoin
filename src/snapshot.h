// Copyright (c) 2026 The NetCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef NETCOIN_SNAPSHOT_H
#define NETCOIN_SNAPSHOT_H

#include "uint256.h"

#include <stdint.h>
#include <string>

#include <boost/filesystem/path.hpp>

/**
 * Block chain snapshots: a .zip with the block files (blk000?.dat) and the
 * block index database (txleveldb/) of a stopped node, plus snapshot.txt:
 *
 *   netcoin-snapshot 1
 *   height=<height of the best block>
 *   hash=<hash of the best block>
 *   created=<date, informational>
 *
 * Importing replaces the block chain of this node (the old one is moved to
 * a chain-backup-<date> folder); the wallet is not touched. After loading,
 * the best block must be the one named in snapshot.txt.
 */
struct SnapshotInfo
{
    int nHeight;
    uint256 hash;
    std::string strCreated;
    uint64_t nDataSize;   // uncompressed size of the files
    bool fKnown;          // height and hash are in the built-in list

    SnapshotInfo() : nHeight(-1), nDataSize(0), fKnown(false) {}
};

/** Read snapshot.txt and check the archive's file list. */
bool ReadSnapshotInfo(const boost::filesystem::path& file, SnapshotInfo& info, std::string& strError);

/** Height and hash of a snapshot published with this version. */
bool IsKnownSnapshot(int nHeight, const uint256& hash);

/**
 * Extract the snapshot into the data directory. Must be called before the
 * block database is opened. The previous block files and txleveldb are
 * moved to pathBackup (empty if there was no previous block chain).
 */
bool ImportSnapshot(const boost::filesystem::path& file, SnapshotInfo& info,
                    boost::filesystem::path& pathBackup, std::string& strError);

#endif
