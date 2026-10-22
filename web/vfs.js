/**
 * Virtual File System (VFS) for Payback 2 Web Port.
 * Features:
 * - Two-tier caching: In-Memory Map (Tier 1) + IndexedDB (Tier 2).
 * - Automatic background persistence.
 * - Binary range slice support.
 */

const DB_NAME = "Payback2_VFS";
const STORE_NAME = "asset_cache";
const DB_VERSION = 1;

export class VirtualFileSystem {
  constructor(baseUri = "./assets/") {
    this.baseUri = baseUri;
    this.memoryCache = new Map();
    this.db = null;
    this._dbReady = this.initIndexedDB();
  }

  async initIndexedDB() {
    if (!window.indexedDB) {
      console.warn("[vfs] IndexedDB not available, using in-memory only.");
      return;
    }

    return new Promise((resolve) => {
      const request = indexedDB.open(DB_NAME, DB_VERSION);
      request.onupgradeneeded = (e) => {
        const db = e.target.result;
        if (!db.objectStoreNames.contains(STORE_NAME)) {
          db.createObjectStore(STORE_NAME);
        }
      };
      request.onsuccess = (e) => {
        this.db = e.target.result;
        console.log("[vfs] IndexedDB cache initialized.");
        resolve(this.db);
      };
      request.onerror = (e) => {
        console.warn("[vfs] IndexedDB open error:", e);
        resolve(null);
      };
    });
  }

  async getFromDB(path) {
    await this._dbReady;
    if (!this.db) return null;

    return new Promise((resolve) => {
      try {
        const tx = this.db.transaction(STORE_NAME, "readonly");
        const store = tx.objectStore(STORE_NAME);
        const req = store.get(path);
        req.onsuccess = () => resolve(req.result ? new Uint8Array(req.result) : null);
        req.onerror = () => resolve(null);
      } catch (e) {
        resolve(null);
      }
    });
  }

  async saveToDB(path, uint8Array) {
    await this._dbReady;
    if (!this.db) return;

    try {
      const tx = this.db.transaction(STORE_NAME, "readwrite");
      const store = tx.objectStore(STORE_NAME);
      store.put(uint8Array.buffer, path);
    } catch (e) {
      // Ignore quota errors silently
    }
  }

  async loadAsset(path) {
    // 1. Check in-memory cache
    if (this.memoryCache.has(path)) {
      return this.memoryCache.get(path);
    }

    // 2. Check IndexedDB persistent cache
    const idbData = await this.getFromDB(path);
    if (idbData) {
      this.memoryCache.set(path, idbData);
      return idbData;
    }

    // 3. Network fetch
    try {
      const cleanPath = path.startsWith("/") ? path.slice(1) : path;
      const resp = await fetch(`${this.baseUri}${cleanPath}`);
      if (!resp.ok) {
        console.warn(`[vfs] Asset not found: ${cleanPath} (HTTP ${resp.status})`);
        return null;
      }
      const arrayBuffer = await resp.arrayBuffer();
      const uint8 = new Uint8Array(arrayBuffer);

      this.memoryCache.set(path, uint8);
      this.saveToDB(path, uint8);
      return uint8;
    } catch (err) {
      console.warn(`[vfs] Failed to fetch asset ${path}:`, err);
      return null;
    }
  }

  has(path) {
    return this.memoryCache.has(path);
  }

  get(path) {
    return this.memoryCache.get(path) || null;
  }
}
