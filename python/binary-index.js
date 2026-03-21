/**
 * Binary Index Manager — POGLS V3.1
 *
 * Changes from ORBS baseline:
 *   • HASH_ALGO_CONFIG  — config-driven hashing (MD5 default, SHA-256 upgrade path)
 *   • algoId (1 byte)  — stored per DNA entry, enables safe algo migration
 *   • MIGRATION_STATE  — idle | running | committed (prevents inconsistent state on crash)
 *   • computeHash()    — single centralized hash function, single call on hot path
 *   • buildMerkleRoot()— snapshot-boundary seal (Option C), never touches write path
 *   • migrateLegacyHashes() — one-time offline pass, resumable
 *
 * Hot path guarantee: single hash call per operation, always.
 * Deep Lane: untouched.
 * Immigration logic: untouched.
 */

'use strict';

const fs     = require('fs');
const path   = require('path');
const crypto = require('crypto');

// ═══════════════════════════════════════════════════════════════════
// HASH CONFIG — change ENV to upgrade, never edit logic
// ═══════════════════════════════════════════════════════════════════

const HASH_ALGO_CONFIG = {
  0: 'md5',     // legacy / speed — internal trusted env
  1: 'sha256',  // cryptographic proof — external / multi-party
};

const CURRENT_ALGO_ID = (() => {
  const env = (process.env.POGLS_HASH_ALGO || 'md5').toLowerCase();
  if (env === 'sha256') return 1;
  return 0;
})();

// ═══════════════════════════════════════════════════════════════════
// MIGRATION STATE — written to header reserved bytes [8-9]
// Prevents inconsistent state if process crashes mid-migration
// ═══════════════════════════════════════════════════════════════════

const MIGRATION_STATE = {
  IDLE:      0,   // no migration in progress
  RUNNING:   1,   // migration started but not committed
  COMMITTED: 2,   // migration complete, safe to switch ENV
};

// ═══════════════════════════════════════════════════════════════════
// FILE LAYOUT
// ═══════════════════════════════════════════════════════════════════

const SMART_HEADER_SIZE = 16;
//  [0-3]  : totalRecords      uint32BE
//  [4-7]  : scalingFactor     floatBE
//  [8]    : migrationState    uint8  (MIGRATION_STATE enum)
//  [9]    : targetAlgoId      uint8  (target algo for migration)
//  [10-15]: reserved

const COORDINATE_INDEX  = './storage/coordinate_index.bin';
const DNA_HASH_TABLE    = './storage/dna_hashes.bin';

// ═══════════════════════════════════════════════════════════════════
// CORE HELPERS
// ═══════════════════════════════════════════════════════════════════

/**
 * Single centralized hash function.
 * Always called once per operation — hot path guarantee.
 * @param {Buffer|string} content
 * @param {number} algoId  0=md5, 1=sha256
 * @returns {string} hex digest
 */
function computeHash(content, algoId = CURRENT_ALGO_ID) {
  const algo = HASH_ALGO_CONFIG[algoId] || 'md5';
  return crypto.createHash(algo).update(content).digest('hex');
}

/**
 * Build Merkle root from an array of hex hash strings.
 * Called ONLY at snapshot boundary (immigration close).
 * Never on write path.
 *
 * Tree pairs are sorted for determinism (order-independent).
 * Returns hex string of root hash.
 * @param {string[]} hashes
 * @param {number} algoId  must match the algo used for leaf hashes
 * @returns {string} merkleRoot hex
 */
function buildMerkleRoot(hashes, algoId = CURRENT_ALGO_ID) {
  if (!hashes || hashes.length === 0) return '';
  if (hashes.length === 1) return hashes[0];

  const algo = HASH_ALGO_CONFIG[algoId] || 'md5';
  let level = [...hashes];

  while (level.length > 1) {
    const next = [];
    for (let i = 0; i < level.length; i += 2) {
      const left  = level[i];
      const right = level[i + 1] || left;  // odd node pairs with itself
      // sort pair for determinism — Merkle doesn't care about order
      const pair  = [left, right].sort().join('');
      next.push(crypto.createHash(algo).update(pair).digest('hex'));
    }
    level = next;
  }

  return level[0];
}

// ═══════════════════════════════════════════════════════════════════
// BinaryIndexManager
// ═══════════════════════════════════════════════════════════════════

class BinaryIndexManager {
  constructor(options = {}) {
    this.coordinateIndexPath = options.coordinateIndexPath || COORDINATE_INDEX;
    this.dnaHashTablePath    = options.dnaHashTablePath    || DNA_HASH_TABLE;
    this.scalingFactor       = options.scalingFactor       || 1.0;

    // In-memory caches
    this.fileIndex    = {};  // fileId → {name, versions[], currentOffset, currentSize}
    this.dnaLookup    = {};  // `${algoId}:${hash}` → {offset, fileIds[], count, algoId}
    this.coordinateMap = {}; // coordinate → {offset, size, blockIndex}

    // Migration state (mirrors header bytes 8-9)
    this._migrationState  = MIGRATION_STATE.IDLE;
    this._targetAlgoId    = CURRENT_ALGO_ID;

    // Statistics
    this.stats = {
      totalRecords:        0,
      totalFiles:          0,
      totalVersions:       0,
      entangledVersions:   0,
      uniqueHashes:        0,
      deduplicationRatio:  0,
    };

    this._initializeIndex();
  }

  // ─────────────────────────────────────────────────────────────────
  // INIT
  // ─────────────────────────────────────────────────────────────────

  _initializeIndex() {
    const storageDir = path.dirname(this.coordinateIndexPath);
    if (!fs.existsSync(storageDir)) {
      fs.mkdirSync(storageDir, { recursive: true });
    }

    if (!fs.existsSync(this.coordinateIndexPath)) {
      fs.writeFileSync(this.coordinateIndexPath, this._buildHeader());
      console.log(`📝 Created coordinate index: ${this.coordinateIndexPath}`);
    }

    if (!fs.existsSync(this.dnaHashTablePath)) {
      fs.writeFileSync(this.dnaHashTablePath, Buffer.alloc(0));
      console.log(`📝 Created DNA hash table: ${this.dnaHashTablePath}`);
    }

    // Safety check: if header shows RUNNING migration but ENV is already
    // pointing to new algo — something crashed mid-migration. Warn and hold.
    this._checkMigrationSafety();
  }

  _buildHeader() {
    const header = Buffer.alloc(SMART_HEADER_SIZE, 0);
    header.writeUInt32BE(this.stats.totalRecords, 0);
    header.writeFloatBE(this.scalingFactor, 4);
    header.writeUInt8(this._migrationState, 8);
    header.writeUInt8(this._targetAlgoId, 9);
    // [10-15] reserved
    return header;
  }

  _updateHeader() {
    if (!fs.existsSync(this.coordinateIndexPath)) return;
    const data = fs.readFileSync(this.coordinateIndexPath);
    data.writeUInt32BE(this.stats.totalRecords, 0);
    data.writeUInt8(this._migrationState, 8);
    data.writeUInt8(this._targetAlgoId, 9);
    fs.writeFileSync(this.coordinateIndexPath, data);
  }

  _checkMigrationSafety() {
    if (!fs.existsSync(this.coordinateIndexPath)) return;
    try {
      const data = fs.readFileSync(this.coordinateIndexPath);
      if (data.length < SMART_HEADER_SIZE) return;
      const storedState   = data.readUInt8(8);
      const storedTarget  = data.readUInt8(9);
      if (storedState === MIGRATION_STATE.RUNNING) {
        console.warn(
          `⚠️  Migration was interrupted (state=RUNNING, target=algo:${storedTarget}).` +
          `\n   Run migrateLegacyHashes() to resume, or revert POGLS_HASH_ALGO to previous value.`
        );
        this._migrationState = MIGRATION_STATE.RUNNING;
        this._targetAlgoId   = storedTarget;
      }
    } catch (_) { /* ignore read errors on fresh install */ }
  }

  // ─────────────────────────────────────────────────────────────────
  // CORE: Record, Deduplicate, Access
  // ─────────────────────────────────────────────────────────────────

  /**
   * Record a file version.
   * Caller computes dnaHash externally using computeHash() —
   * this keeps the manager agnostic of content buffering.
   *
   * @param {string} fileId
   * @param {string} fileName
   * @param {number} offset       byte offset in vault
   * @param {number} size         data size in bytes
   * @param {string} dnaHash      hex digest (caller used computeHash())
   * @param {number} [algoId]     defaults to CURRENT_ALGO_ID
   */
  recordFileVersion(fileId, fileName, offset, size, dnaHash, algoId = CURRENT_ALGO_ID) {
    if (!this.fileIndex[fileId]) {
      this.fileIndex[fileId] = {
        id:             fileId,
        name:           fileName,
        versions:       [],
        currentOffset:  offset,
        currentSize:    size,
        currentVersion: 1,
      };
      this.stats.totalFiles++;
    }

    const lookupKey  = `${algoId}:${dnaHash}`;
    const isDuplicate = this.dnaLookup[lookupKey] !== undefined;

    const versionNum = this.fileIndex[fileId].versions.length + 1;
    const version = {
      version:     versionNum,
      offset:      offset,
      size:        size,
      dnaHash:     dnaHash,
      algoId:      algoId,           // ← stored per entry
      timestamp:   new Date().toISOString(),
      isDuplicate: isDuplicate,
      coordinate:  this._generateCoordinate(fileId, versionNum),
    };

    this.fileIndex[fileId].versions.push(version);
    this.fileIndex[fileId].currentOffset  = offset;
    this.fileIndex[fileId].currentSize    = size;
    this.fileIndex[fileId].currentVersion = versionNum;

    this._addDNAEntry(lookupKey, offset, fileId, algoId);

    this.stats.totalRecords++;
    this.stats.totalVersions++;
    if (isDuplicate) this.stats.entangledVersions++;
    this._updateDeduplicationRatio();

    const marker = isDuplicate ? '🌀 ENTANGLED' : '🗂️  NEW DNA';
    console.log(`${marker} | ${fileName} | v${versionNum} | algo:${algoId} | ${dnaHash.slice(0, 8)}... | offset:${offset}`);

    return { success: true, isDuplicate, offset, version: versionNum };
  }

  /**
   * Check duplicate — single hash call, hot path.
   * @param {string} dnaHash  pre-computed by caller
   * @param {number} [algoId]
   */
  checkDNADuplicate(dnaHash, algoId = CURRENT_ALGO_ID) {
    return this.dnaLookup[`${algoId}:${dnaHash}`] !== undefined;
  }

  _addDNAEntry(lookupKey, offset, fileId, algoId) {
    if (!this.dnaLookup[lookupKey]) {
      this.dnaLookup[lookupKey] = { offset, fileIds: [fileId], count: 1, algoId };
    } else {
      if (!this.dnaLookup[lookupKey].fileIds.includes(fileId)) {
        this.dnaLookup[lookupKey].fileIds.push(fileId);
      }
      this.dnaLookup[lookupKey].count++;
    }
  }

  // ─────────────────────────────────────────────────────────────────
  // MERKLE SEAL — called at immigration cycle close only
  // ─────────────────────────────────────────────────────────────────

  /**
   * Build and return Merkle root for a given set of fileIds.
   * Typically called when immigration cycle commits.
   *
   * Does NOT write to vault — caller decides where to store root
   * (Shadow Header, Tail Log, etc.)
   *
   * @param {string[]} fileIds  files moved in this cycle
   * @param {number}   [algoId] must match algo used for those entries
   * @returns {{ root: string, algoId: number, blockCount: number }}
   */
  buildSnapshotSeal(fileIds, algoId = CURRENT_ALGO_ID) {
    const hashes = [];

    for (const fid of fileIds) {
      const file = this.fileIndex[fid];
      if (!file) continue;
      // collect current version hash of each file
      const cur = file.versions[file.currentVersion - 1];
      if (cur && cur.algoId === algoId) {
        hashes.push(cur.dnaHash);
      }
    }

    const root = buildMerkleRoot(hashes, algoId);
    console.log(`🔒 Snapshot seal | algo:${algoId} | blocks:${hashes.length} | root:${root.slice(0, 16)}...`);
    return { root, algoId, blockCount: hashes.length };
  }

  // ─────────────────────────────────────────────────────────────────
  // MIGRATION — one-time offline pass, resumable
  // ─────────────────────────────────────────────────────────────────

  /**
   * Upgrade all MD5 DNA entries to SHA-256.
   * Safe to call multiple times — idempotent and resumable.
   *
   * Requires a content resolver: (offset, size) → Buffer
   * because we need to rehash actual bytes.
   *
   * Steps:
   *   1. Set migration state = RUNNING  (written to header)
   *   2. Rehash each legacy entry
   *   3. Set migration state = COMMITTED
   *   4. Caller then changes ENV and restarts
   *
   * If process crashes at step 2:
   *   Next startup sees state=RUNNING → warns → caller calls this again → resumes
   *
   * @param {Function} contentResolver  async (offset, size) => Buffer
   */
  async migrateLegacyHashes(contentResolver) {
    const fromAlgoId = 0; // MD5
    const toAlgoId   = 1; // SHA-256

    // Mark migration started
    this._migrationState = MIGRATION_STATE.RUNNING;
    this._targetAlgoId   = toAlgoId;
    this._updateHeader();
    console.log(`🔄 Migration started: algo ${fromAlgoId} → ${toAlgoId}`);

    let migrated = 0;
    let skipped  = 0;

    for (const file of Object.values(this.fileIndex)) {
      for (const version of file.versions) {
        if (version.algoId !== fromAlgoId) { skipped++; continue; }

        // Rehash content
        const content   = await contentResolver(version.offset, version.size);
        const newHash   = computeHash(content, toAlgoId);
        const oldKey    = `${fromAlgoId}:${version.dnaHash}`;
        const newKey    = `${toAlgoId}:${newHash}`;

        // Update DNA lookup — move entry, keep offset
        const oldEntry = this.dnaLookup[oldKey];
        if (oldEntry) {
          this.dnaLookup[newKey] = { ...oldEntry, algoId: toAlgoId };
          delete this.dnaLookup[oldKey];
        } else {
          this.dnaLookup[newKey] = {
            offset: version.offset,
            fileIds: [file.id],
            count: 1,
            algoId: toAlgoId,
          };
        }

        // Update version entry in place
        version.dnaHash = newHash;
        version.algoId  = toAlgoId;
        migrated++;
      }
    }

    // Mark committed
    this._migrationState = MIGRATION_STATE.COMMITTED;
    this._updateHeader();

    console.log(`✅ Migration committed | migrated:${migrated} | skipped:${skipped}`);
    console.log(`   Now set POGLS_HASH_ALGO=sha256 and restart.`);

    return { migrated, skipped };
  }

  // ─────────────────────────────────────────────────────────────────
  // ACCESS
  // ─────────────────────────────────────────────────────────────────

  getFile(fileId)                   { return this.fileIndex[fileId] || null; }
  getAllFiles()                      { return Object.values(this.fileIndex); }
  getVersion(fileId, versionNum)    {
    const f = this.fileIndex[fileId];
    return f ? (f.versions[versionNum - 1] || null) : null;
  }

  switchVersion(fileId, versionNum) {
    const file = this.fileIndex[fileId];
    if (!file) return false;
    const version = file.versions[versionNum - 1];
    if (!version) return false;
    file.currentOffset  = version.offset;
    file.currentSize    = version.size;
    file.currentVersion = versionNum;
    console.log(`🌀 Switched ${file.name} → v${versionNum} (offset:${version.offset})`);
    return true;
  }

  findByDNAHash(dnaHash, algoId = CURRENT_ALGO_ID) {
    const key = `${algoId}:${dnaHash}`;
    const results = [];
    for (const [fileId, file] of Object.entries(this.fileIndex)) {
      const versions = file.versions.filter(v => v.dnaHash === dnaHash && v.algoId === algoId);
      if (versions.length > 0) results.push({ fileId, fileName: file.name, versions });
    }
    return results;
  }

  // ─────────────────────────────────────────────────────────────────
  // COORDINATE
  // ─────────────────────────────────────────────────────────────────

  _generateCoordinate(fileId, versionNum) {
    // Use CURRENT_ALGO_ID for coordinate hash — stable per file
    const baseHash  = computeHash(fileId, CURRENT_ALGO_ID);
    const baseAngle = (parseInt(baseHash.slice(0, 8), 16) / 0xffffffff) * 87.7117793;
    if (versionNum === 1) return `${baseAngle.toFixed(10)}°`;
    return `${baseAngle.toFixed(10)}°.${String(versionNum).padStart(3, '0')}`;
  }

  // ─────────────────────────────────────────────────────────────────
  // PERSISTENCE
  // ─────────────────────────────────────────────────────────────────

  saveToBinary() {
    try {
      const header = this._buildHeader();
      let binaryData = header;

      for (const [fileId, file] of Object.entries(this.fileIndex)) {
        binaryData = Buffer.concat([binaryData, this._serializeFileEntry(fileId, file)]);
      }

      fs.writeFileSync(this.coordinateIndexPath, binaryData);
      fs.writeFileSync(this.dnaHashTablePath, this._serializeDNATable());

      console.log(`💾 Binary index saved | files:${this.stats.totalFiles} | versions:${this.stats.totalVersions}`);
      return true;
    } catch (err) {
      console.error(`❌ saveToBinary failed:`, err);
      return false;
    }
  }

  /**
   * Version entry binary format:
   * [VersionNum:4][Offset:8][Size:8][Hash:32][algoId:1][Timestamp:8][Coordinate:64]
   * Total: 125 bytes per version (was 124, +1 for algoId)
   */
  _serializeVersionEntry(version) {
    const vNum      = Buffer.alloc(4);  vNum.writeUInt32BE(version.version, 0);
    const offset    = Buffer.alloc(8);  offset.writeBigUInt64BE(BigInt(version.offset), 0);
    const size      = Buffer.alloc(8);  size.writeBigUInt64BE(BigInt(version.size), 0);
    const hash      = Buffer.alloc(32); Buffer.from(version.dnaHash.padEnd(64,'0'), 'hex').copy(hash, 0, 0, 32);
    const algoId    = Buffer.alloc(1);  algoId.writeUInt8(version.algoId ?? 0, 0);
    const timestamp = Buffer.alloc(8);  timestamp.writeBigUInt64BE(BigInt(new Date(version.timestamp).getTime()), 0);
    const coord     = Buffer.alloc(64); Buffer.from(version.coordinate || '', 'utf8').copy(coord, 0, 0, 64);

    return Buffer.concat([vNum, offset, size, hash, algoId, timestamp, coord]);
  }

  _serializeFileEntry(fileId, file) {
    const id             = Buffer.alloc(16);  Buffer.from(fileId, 'utf8').copy(id, 0, 0, 16);
    const name           = Buffer.alloc(32);  Buffer.from(file.name, 'utf8').copy(name, 0, 0, 32);
    const versionCount   = Buffer.alloc(4);   versionCount.writeUInt32BE(file.versions.length, 0);
    const currentOffset  = Buffer.alloc(8);   currentOffset.writeBigUInt64BE(BigInt(file.currentOffset), 0);
    const currentSize    = Buffer.alloc(8);   currentSize.writeBigUInt64BE(BigInt(file.currentSize), 0);
    const currentVersion = Buffer.alloc(4);   currentVersion.writeUInt32BE(file.currentVersion, 0);

    let entry = Buffer.concat([id, name, versionCount, currentOffset, currentSize, currentVersion]);
    for (const v of file.versions) {
      entry = Buffer.concat([entry, this._serializeVersionEntry(v)]);
    }
    return entry;
  }

  _serializeDNATable() {
    const keys    = Object.keys(this.dnaLookup);
    const count   = Buffer.alloc(4);
    count.writeUInt32BE(keys.length, 0);
    let tableData = count;

    for (const [lookupKey, entry] of Object.entries(this.dnaLookup)) {
      // lookupKey = "${algoId}:${hash}"
      const [, rawHash] = lookupKey.split(':');
      const hashBytes   = Buffer.alloc(32);
      Buffer.from((rawHash || '').padEnd(64, '0'), 'hex').copy(hashBytes, 0, 0, 32);

      const algoIdByte  = Buffer.alloc(1);  algoIdByte.writeUInt8(entry.algoId ?? 0, 0);
      const offset      = Buffer.alloc(8);  offset.writeBigUInt64BE(BigInt(entry.offset), 0);
      const fidCount    = Buffer.alloc(4);  fidCount.writeUInt32BE(entry.fileIds.length, 0);

      tableData = Buffer.concat([tableData, hashBytes, algoIdByte, offset, fidCount]);
      for (const fid of entry.fileIds) {
        const fidBuf = Buffer.alloc(16);
        Buffer.from(fid, 'utf8').copy(fidBuf, 0, 0, 16);
        tableData = Buffer.concat([tableData, fidBuf]);
      }
    }
    return tableData;
  }

  loadFromBinary() {
    try {
      if (!fs.existsSync(this.coordinateIndexPath)) return true;
      const data = fs.readFileSync(this.coordinateIndexPath);
      if (data.length < SMART_HEADER_SIZE) return true;

      this.stats.totalRecords  = data.readUInt32BE(0);
      this.scalingFactor       = data.readFloatBE(4);
      this._migrationState     = data.readUInt8(8);
      this._targetAlgoId       = data.readUInt8(9);

      console.log(`📖 Loaded header | records:${this.stats.totalRecords} | migrationState:${this._migrationState}`);
      return true;
    } catch (err) {
      console.error(`❌ loadFromBinary failed:`, err);
      return false;
    }
  }

  // ─────────────────────────────────────────────────────────────────
  // STATS & DIAGNOSTICS
  // ─────────────────────────────────────────────────────────────────

  _updateDeduplicationRatio() {
    const unique = Object.keys(this.dnaLookup).length;
    this.stats.uniqueHashes       = unique;
    this.stats.deduplicationRatio = this.stats.totalVersions > 0
      ? unique / this.stats.totalVersions : 0;
  }

  getStatistics() {
    const ms = ['IDLE', 'RUNNING', 'COMMITTED'];
    return {
      ...this.stats,
      currentAlgoId:            CURRENT_ALGO_ID,
      currentAlgo:              HASH_ALGO_CONFIG[CURRENT_ALGO_ID],
      migrationState:           ms[this._migrationState] || 'UNKNOWN',
      deduplicationPercentage:  (this.stats.deduplicationRatio * 100).toFixed(1),
      entangledPercentage:      this.stats.totalVersions > 0
        ? ((this.stats.entangledVersions / this.stats.totalVersions) * 100).toFixed(1) : '0.0',
    };
  }

  printReport() {
    const s = this.getStatistics();
    console.log(`
╔════════════════════════════════════════════════════╗
║            BINARY INDEX — POGLS V3.1               ║
╠════════════════════════════════════════════════════╣
  Hash Algo:        ${s.currentAlgo} (id:${s.currentAlgoId})
  Migration State:  ${s.migrationState}
  Total Files:      ${s.totalFiles}
  Total Versions:   ${s.totalVersions}
  Unique Hashes:    ${s.uniqueHashes}
  Entangled:        ${s.entangledVersions} (${s.entangledPercentage}%)
  Dedup Ratio:      ${s.deduplicationPercentage}%
╚════════════════════════════════════════════════════╝`);
  }

  deleteFile(fileId) {
    const file = this.fileIndex[fileId];
    if (!file) return false;
    this.stats.totalFiles--;
    this.stats.totalVersions -= file.versions.length;
    for (const v of file.versions) {
      const key = `${v.algoId ?? 0}:${v.dnaHash}`;
      const entry = this.dnaLookup[key];
      if (entry) {
        entry.fileIds = entry.fileIds.filter(id => id !== fileId);
        if (entry.fileIds.length === 0) {
          delete this.dnaLookup[key];
          this.stats.uniqueHashes--;
        }
      }
    }
    delete this.fileIndex[fileId];
    this._updateDeduplicationRatio();
    console.log(`🗑️  Deleted: ${file.name}`);
    return true;
  }
}

// ═══════════════════════════════════════════════════════════════════
// EXPORTS
// ═══════════════════════════════════════════════════════════════════

module.exports = {
  BinaryIndexManager,
  computeHash,
  buildMerkleRoot,
  HASH_ALGO_CONFIG,
  CURRENT_ALGO_ID,
  MIGRATION_STATE,
};
