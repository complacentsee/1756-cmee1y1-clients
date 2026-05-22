// SPDX-License-Identifier: MIT

package ocxbp

import "sync"

// tagCache is the per-PLC symbol cache, shared across all TagDB
// handles to the same path on a Client.  Mirrors C's struct
// bp_tag_cache (c/src/proto.h) and Python's _TagCache.
//
// Lifecycle:
//   - TagDB.Build invalidates the cache for db.path and writes
//     totalCount from the engine's response.
//   - TagDB.LookupSymbol does a linear scan; on miss it walks
//     SymbolAt(knownCount..totalCount-1) until the name matches,
//     appending each examined symbol to symbols[].
//   - TagDB.PreloadSymbols walks the entire table eagerly.
type tagCache struct {
	mu          sync.Mutex
	symbols     []SymbolInfo // len == capCount once Build has populated
	knownCount  int          // number of valid entries at the front
	totalCount  int          // from BuildTagDb
}

// findOrAllocCache returns (or creates) the cache for path.
func (c *Client) findOrAllocCache(path string) *tagCache {
	c.tagCacheMu.Lock()
	defer c.tagCacheMu.Unlock()
	tc, ok := c.tagCaches[path]
	if !ok {
		tc = &tagCache{}
		c.tagCaches[path] = tc
	}
	return tc
}

// findCache returns the cache for path or nil if none exists.
func (c *Client) findCache(path string) *tagCache {
	c.tagCacheMu.Lock()
	defer c.tagCacheMu.Unlock()
	return c.tagCaches[path]
}

// resetCacheAfterBuild is called from TagDB.Build after a successful
// walk.  Resizes the cache for `path` to hold totalCount entries and
// clears knownCount so the lazy fill rebuilds.
func (c *Client) resetCacheAfterBuild(path string, totalCount uint16) {
	tc := c.findOrAllocCache(path)
	tc.mu.Lock()
	defer tc.mu.Unlock()
	tc.symbols = nil
	tc.knownCount = 0
	tc.totalCount = int(totalCount)
	if totalCount > 0 {
		tc.symbols = make([]SymbolInfo, totalCount)
	}
}

// LookupSymbol returns the descriptor for `name` from the per-client
// cache, fetching from the PLC as needed.  First lookup after Build
// for a previously-unseen name walks SymbolAt incrementally.  Returns
// ErrParamRange if the name isn't in the PLC's tag table.
func (db *TagDB) LookupSymbol(name string) (SymbolInfo, error) {
	if db == nil || db.client == nil {
		return SymbolInfo{}, ErrNullArg
	}
	tc := db.client.findOrAllocCache(db.path)

	tc.mu.Lock()
	if tc.totalCount == 0 && len(tc.symbols) == 0 {
		tc.mu.Unlock()
		return SymbolInfo{}, ErrParamRange
	}
	// Linear scan known entries first.
	for i := 0; i < tc.knownCount; i++ {
		if tc.symbols[i].Name == name {
			out := tc.symbols[i]
			tc.mu.Unlock()
			return out, nil
		}
	}
	// Cache miss + room to grow: walk SymbolAt incrementally.  Release
	// the cache mutex around each IPC call so other lookups for
	// already-cached names don't block.
	for tc.knownCount < tc.totalCount {
		idx := uint16(tc.knownCount)
		tc.mu.Unlock()
		sym, err := db.SymbolAt(idx)
		if err != nil {
			return SymbolInfo{}, err
		}
		tc.mu.Lock()
		if int(idx) == tc.knownCount && tc.knownCount < len(tc.symbols) {
			tc.symbols[tc.knownCount] = sym
			tc.knownCount++
		}
		// Check the appended (or parallel-inserted) entries for a name match.
		for i := int(idx); i < tc.knownCount; i++ {
			if tc.symbols[i].Name == name {
				out := tc.symbols[i]
				tc.mu.Unlock()
				return out, nil
			}
		}
	}
	tc.mu.Unlock()
	return SymbolInfo{}, ErrParamRange
}

// PreloadSymbols eagerly fetches every symbol descriptor and
// populates the cache.  Returns the count of cached entries on
// success.  Useful when callers want to pay the cost up-front
// instead of amortized across the first ReadTags / WriteTags.
func (db *TagDB) PreloadSymbols() (int, error) {
	if db == nil || db.client == nil {
		return 0, ErrNullArg
	}
	tc := db.client.findOrAllocCache(db.path)
	tc.mu.Lock()
	if tc.totalCount == 0 && len(tc.symbols) == 0 {
		tc.mu.Unlock()
		return 0, ErrParamRange
	}
	for tc.knownCount < tc.totalCount {
		idx := uint16(tc.knownCount)
		tc.mu.Unlock()
		sym, err := db.SymbolAt(idx)
		if err != nil {
			return int(idx), err
		}
		tc.mu.Lock()
		if int(idx) == tc.knownCount && tc.knownCount < len(tc.symbols) {
			tc.symbols[tc.knownCount] = sym
			tc.knownCount++
		}
	}
	known := tc.knownCount
	tc.mu.Unlock()
	return known, nil
}
