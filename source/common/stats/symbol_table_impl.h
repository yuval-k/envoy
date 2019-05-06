#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "envoy/common/exception.h"
#include "envoy/stats/symbol_table.h"

#include "common/common/assert.h"
#include "common/common/hash.h"
#include "common/common/lock_guard.h"
#include "common/common/non_copyable.h"
#include "common/common/stack_array.h"
#include "common/common/thread.h"
#include "common/common/utility.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

namespace Envoy {
namespace Stats {

/** A Symbol represents a string-token with a small index. */
using Symbol = uint32_t;

/**
 * We encode the byte-size of a StatName as its first two bytes.
 */
constexpr uint64_t StatNameSizeEncodingBytes = 2;
constexpr uint64_t StatNameMaxSize = 1 << (8 * StatNameSizeEncodingBytes); // 65536

/** Transient representations of a vector of 32-bit symbols */
using SymbolVec = std::vector<Symbol>;

/**
 * SymbolTableImpl manages a namespace optimized for stats, which are typically
 * composed of arrays of "."-separated tokens, with a significant overlap
 * between the tokens. Each token is mapped to a Symbol (uint32_t) and
 * reference-counted so that no-longer-used symbols can be reclaimed.
 *
 * We use a uint8_t array to encode a "."-deliminated stat-name into arrays of
 * integer symbol symbol IDs in order to conserve space, as in practice the
 * majority of token instances in stat names draw from a fairly small set of
 * common names, typically less than 100. The format is somewhat similar to
 * UTF-8, with a variable-length array of uint8_t. See the implementation for
 * details.
 *
 * StatNameStorage can be used to manage memory for the byte-encoding. Not all
 * StatNames are backed by StatNameStorage -- the storage may be inlined into
 * another object such as HeapStatData. StaNameStorage is not fully RAII --
 * instead the owner must call free(SymbolTable&) explicitly before
 * StatNameStorage is destructed. This saves 8 bytes of storage per stat,
 * relative to holding a SymbolTable& in each StatNameStorage object.
 *
 * A StatName is a copyable and assignable reference to this storage. It does
 * not own the storage or keep it alive via reference counts; the owner must
 * ensure the backing store lives as long as the StatName.
 *
 * The underlying Symbol / SymbolVec data structures are private to the
 * impl. One side effect of the non-monotonically-increasing symbol counter is
 * that if a string is encoded, the resulting stat is destroyed, and then that
 * same string is re-encoded, it may or may not encode to the same underlying
 * symbol.
 */
class SymbolTableImpl : public SymbolTable {
public:
  /**
   * Intermediate representation for a stat-name. This helps store multiple
   * names in a single packed allocation. First we encode each desired name,
   * then sum their sizes for the single packed allocation. This is used to
   * store MetricImpl's tags and tagExtractedName.
   */
  class Encoding {
  public:
    /**
     * Before destructing SymbolEncoding, you must call moveToStorage. This
     * transfers ownership, and in particular, the responsibility to call
     * SymbolTable::clear() on all referenced symbols. If we ever wanted
     * to be able to destruct a SymbolEncoding without transferring it
     * we could add a clear(SymbolTable&) method.
     */
    ~Encoding();

    /**
     * Encodes a token into the vec.
     *
     * @param symbol the symbol to encode.
     */
    void addSymbol(Symbol symbol);

    /**
     * Decodes a uint8_t array into a SymbolVec.
     */
    static SymbolVec decodeSymbols(const SymbolTable::Storage array, uint64_t size);

    /**
     * Returns the number of bytes required to represent StatName as a uint8_t
     * array, including the encoded size.
     */
    uint64_t bytesRequired() const { return dataBytesRequired() + StatNameSizeEncodingBytes; }

    /**
     * @return the number of uint8_t entries we collected while adding symbols.
     */
    uint64_t dataBytesRequired() const { return vec_.size(); }

    /**
     * Moves the contents of the vector into an allocated array. The array
     * must have been allocated with bytesRequired() bytes.
     *
     * @param array destination memory to receive the encoded bytes.
     * @return uint64_t the number of bytes transferred.
     */
    uint64_t moveToStorage(SymbolTable::Storage array);

  private:
    std::vector<uint8_t> vec_;
  };

  SymbolTableImpl();
  ~SymbolTableImpl() override;

  // SymbolTable
  std::string toString(const StatName& stat_name) const override;
  uint64_t numSymbols() const override;
  bool lessThan(const StatName& a, const StatName& b) const override;
  void free(const StatName& stat_name) override;
  void incRefCount(const StatName& stat_name) override;
  StoragePtr join(const std::vector<StatName>& stat_names) const override;
  void populateList(const absl::string_view* names, uint32_t num_names,
                    StatNameList& list) override;
  StoragePtr encode(absl::string_view name) override;
  void callWithStringView(StatName stat_name,
                          const std::function<void(absl::string_view)>& fn) const override;

#ifndef ENVOY_CONFIG_COVERAGE
  void debugPrint() const override;
#endif

  /**
   * Saves the specified length into the byte array, returning the next byte.
   * There is no guarantee that bytes will be aligned, so we can't cast to a
   * uint16_t* and assign, but must individually copy the bytes.
   *
   * @param length the length in bytes to write. Must be < StatNameMaxSize.
   * @param bytes the pointer into which to write the length.
   * @return the pointer to the next byte for writing the data.
   */
  static inline uint8_t* writeLengthReturningNext(uint64_t length, uint8_t* bytes) {
    ASSERT(length < StatNameMaxSize);
    *bytes++ = length & 0xff;
    *bytes++ = length >> 8;
    return bytes;
  }

private:
  friend class StatName;
  friend class StatNameTest;

  struct SharedSymbol {
    SharedSymbol(Symbol symbol) : symbol_(symbol), ref_count_(1) {}

    Symbol symbol_;
    uint32_t ref_count_;
  };

  // This must be held during both encode() and free().
  mutable Thread::MutexBasicLockable lock_;

  /**
   * Decodes a vector of symbols back into its period-delimited stat name. If
   * decoding fails on any part of the symbol_vec, we release_assert and crash
   * hard, since this should never happen, and we don't want to continue running
   * with a corrupt stats set.
   *
   * @param symbols the vector of symbols to decode.
   * @return std::string the retrieved stat name.
   */
  std::string decodeSymbolVec(const SymbolVec& symbols) const;

  /**
   * Convenience function for encode(), symbolizing one string segment at a time.
   *
   * @param sv the individual string to be encoded as a symbol.
   * @return Symbol the encoded string.
   */
  Symbol toSymbol(absl::string_view sv) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  /**
   * Convenience function for decode(), decoding one symbol at a time.
   *
   * @param symbol the individual symbol to be decoded.
   * @return absl::string_view the decoded string.
   */
  absl::string_view fromSymbol(Symbol symbol) const EXCLUSIVE_LOCKS_REQUIRED(lock_);

  /**
   * Stages a new symbol for use. To be called after a successful insertion.
   */
  void newSymbol();

  /**
   * Tokenizes name, finds or allocates symbols for each token, and adds them
   * to encoding.
   *
   * @param name The name to tokenize.
   * @param encoding The encoding to write to.
   */
  void addTokensToEncoding(absl::string_view name, Encoding& encoding);

  Symbol monotonicCounter() {
    Thread::LockGuard lock(lock_);
    return monotonic_counter_;
  }

  // Stores the symbol to be used at next insertion. This should exist ahead of insertion time so
  // that if insertion succeeds, the value written is the correct one.
  Symbol next_symbol_ GUARDED_BY(lock_);

  // If the free pool is exhausted, we monotonically increase this counter.
  Symbol monotonic_counter_;

  // Bitmap implementation.
  // The encode map stores both the symbol and the ref count of that symbol.
  // Using absl::string_view lets us only store the complete string once, in the decode map.
  using EncodeMap = absl::flat_hash_map<absl::string_view, SharedSymbol, StringViewHash>;
  using DecodeMap = absl::flat_hash_map<Symbol, std::unique_ptr<std::string>>;
  EncodeMap encode_map_ GUARDED_BY(lock_);
  DecodeMap decode_map_ GUARDED_BY(lock_);

  // Free pool of symbols for re-use.
  // TODO(ambuc): There might be an optimization here relating to storing ranges of freed symbols
  // using an Envoy::IntervalSet.
  std::stack<Symbol> pool_ GUARDED_BY(lock_);
};

/**
 * Holds backing storage for a StatName. Usage of this is not required, as some
 * applications may want to hold multiple StatName objects in one contiguous
 * uint8_t array, or embed the characters directly in another structure.
 *
 * This is intended for embedding in other data structures that have access
 * to a SymbolTable. StatNameStorage::free(symbol_table) must be called prior
 * to allowing the StatNameStorage object to be destructed, otherwise an assert
 * will fire to guard against symbol-table leaks.
 *
 * Thus this class is inconvenient to directly use as temp storage for building
 * a StatName from a string. Instead it should be used via StatNameManagedStorage.
 */
class StatNameStorage {
public:
  // Basic constructor for when you have a name as a string, and need to
  // generate symbols for it.
  StatNameStorage(absl::string_view name, SymbolTable& table);

  // Move constructor; needed for using StatNameStorage as an
  // absl::flat_hash_map value.
  StatNameStorage(StatNameStorage&& src) : bytes_(std::move(src.bytes_)) {}

  // Obtains new backing storage for an already existing StatName. Used to
  // record a computed StatName held in a temp into a more persistent data
  // structure.
  StatNameStorage(StatName src, SymbolTable& table);

  /**
   * Before allowing a StatNameStorage to be destroyed, you must call free()
   * on it, to drop the references to the symbols, allowing the SymbolTable
   * to shrink.
   */
  ~StatNameStorage();

  /**
   * Decrements the reference counts in the SymbolTable.
   *
   * @param table the symbol table.
   */
  void free(SymbolTable& table);

  /**
   * @return StatName a reference to the owned storage.
   */
  inline StatName statName() const;

private:
  SymbolTable::StoragePtr bytes_;
};

/**
 * Efficiently represents a stat name using a variable-length array of uint8_t.
 * This class does not own the backing store for this array; the backing-store
 * can be held in StatNameStorage, or it can be packed more tightly into another
 * object.
 *
 * When Envoy is configured with a large numbers of clusters, there are a huge
 * number of StatNames, so avoiding extra per-stat pointers has a significant
 * memory impact.
 */
class StatName {
public:
  // Constructs a StatName object directly referencing the storage of another
  // StatName.
  explicit StatName(const SymbolTable::Storage size_and_data) : size_and_data_(size_and_data) {}

  // Constructs an empty StatName object.
  StatName() : size_and_data_(nullptr) {}

  // Constructs a StatName object with new storage, which must be of size
  // src.size(). This is used in the a flow where we first construct a StatName
  // for lookup in a cache, and then on a miss need to store the data directly.
  StatName(const StatName& src, SymbolTable::Storage memory);

  /**
   * Note that this hash function will return a different hash than that of
   * the elaborated string.
   *
   * @return uint64_t a hash of the underlying representation.
   */
  uint64_t hash() const {
    const char* cdata = reinterpret_cast<const char*>(data());
    return HashUtil::xxHash64(absl::string_view(cdata, dataSize()));
  }

  bool operator==(const StatName& rhs) const {
    const uint64_t sz = dataSize();
    return sz == rhs.dataSize() && memcmp(data(), rhs.data(), sz * sizeof(uint8_t)) == 0;
  }
  bool operator!=(const StatName& rhs) const { return !(*this == rhs); }

  /**
   * @return uint64_t the number of bytes in the symbol array, excluding the two-byte
   *                  overhead for the size itself.
   */
  uint64_t dataSize() const {
    return size_and_data_[0] | (static_cast<uint64_t>(size_and_data_[1]) << 8);
  }

  /**
   * @return uint64_t the number of bytes in the symbol array, including the two-byte
   *                  overhead for the size itself.
   */
  uint64_t size() const { return dataSize() + StatNameSizeEncodingBytes; }

  void copyToStorage(SymbolTable::Storage storage) { memcpy(storage, size_and_data_, size()); }

#ifndef ENVOY_CONFIG_COVERAGE
  void debugPrint();
#endif

  /**
   * @return uint8_t* A pointer to the first byte of data (skipping over size bytes).
   */
  const uint8_t* data() const { return size_and_data_ + StatNameSizeEncodingBytes; }

private:
  const uint8_t* size_and_data_;
};

StatName StatNameStorage::statName() const { return StatName(bytes_.get()); }

/**
 * Contains the backing store for a StatName and enough context so it can
 * self-delete through RAII. This works by augmenting StatNameStorage with a
 * reference to the SymbolTable&, so it has an extra 8 bytes of footprint. It
 * is intended to be used in cases where simplicity of implementation is more
 * important than byte-savings, for example:
 *   - outside the stats system
 *   - in tests
 *   - as a scoped temp in a function
 * Due to the extra 8 bytes per instance, scalability should be taken into
 * account before using this as (say) a value or key in a map. In those
 * scenarios, it would be better to store the SymbolTable reference once
 * for the entire map.
 *
 * In the stat structures, we generally use StatNameStorage to avoid the
 * per-stat overhead.
 */
class StatNameManagedStorage : public StatNameStorage {
public:
  // Basic constructor for when you have a name as a string, and need to
  // generate symbols for it.
  StatNameManagedStorage(absl::string_view name, SymbolTable& table)
      : StatNameStorage(name, table), symbol_table_(table) {}

  // Obtains new backing storage for an already existing StatName.
  StatNameManagedStorage(StatName src, SymbolTable& table)
      : StatNameStorage(src, table), symbol_table_(table) {}

  ~StatNameManagedStorage() { free(symbol_table_); }

  SymbolTable& symbolTable() { return symbol_table_; }
  const SymbolTable& symbolTable() const { return symbol_table_; }

private:
  SymbolTable& symbol_table_;
};

// Represents an ordered container of StatNames. The encoding for each StatName
// is byte-packed together, so this carries less overhead than allocating the
// storage separately. The tradeoff is there is no random access; you can only
// iterate through the StatNames.
//
// The maximum size of the list is 255 elements, so the length can fit in a
// byte. It would not be difficult to increase this, but there does not appear
// to be a current need.
class StatNameList {
public:
  ~StatNameList();

  /**
   * @return true if populate() has been called on this list.
   */
  bool populated() const { return storage_ != nullptr; }

  /**
   * Iterates over each StatName in the list, calling f(StatName). f() should
   * return true to keep iterating, or false to end the iteration.
   *
   * @param f The function to call on each stat.
   */
  void iterate(const std::function<bool(StatName)>& f) const;

  /**
   * Frees each StatName in the list. Failure to call this before destruction
   * results in an ASSERT at destruction of the list and the SymbolTable.
   *
   * This is not done as part of destruction as the SymbolTable may already
   * be destroyed.
   *
   * @param symbol_table the symbol table.
   */
  void clear(SymbolTable& symbol_table);

private:
  friend class FakeSymbolTableImpl;
  friend class SymbolTableImpl;

  /**
   * Moves the specified storage into the list. The storage format is an
   * array of bytes, organized like this:
   *
   * [0] The number of elements in the list (must be < 256).
   * [1] low order 8 bits of the number of symbols in the first element.
   * [2] high order 8 bits of the number of symbols in the first element.
   * [3...] the symbols in the first element.
   * ...
   *
   *
   * For FakeSymbolTableImpl, each symbol is a single char, casted into a
   * uint8_t. For SymbolTableImpl, each symbol is 1 or more bytes, in a
   * variable-length encoding. See SymbolTableImpl::Encoding::addSymbol for
   * details.
   */
  void moveStorageIntoList(SymbolTable::StoragePtr&& storage) { storage_ = std::move(storage); }

  SymbolTable::StoragePtr storage_;
};

// Helper class for constructing hash-tables with StatName keys.
struct StatNameHash {
  size_t operator()(const StatName& a) const { return a.hash(); }
};

// Helper class for constructing hash-tables with StatName keys.
struct StatNameCompare {
  bool operator()(const StatName& a, const StatName& b) const { return a == b; }
};

// Value-templatized hash-map with StatName key.
template <class T>
using StatNameHashMap = absl::flat_hash_map<StatName, T, StatNameHash, StatNameCompare>;

// Hash-set of StatNames
using StatNameHashSet = absl::flat_hash_set<StatName, StatNameHash, StatNameCompare>;

// Helper class for sorting StatNames.
struct StatNameLessThan {
  StatNameLessThan(const SymbolTable& symbol_table) : symbol_table_(symbol_table) {}
  bool operator()(const StatName& a, const StatName& b) const {
    return symbol_table_.lessThan(a, b);
  }

  const SymbolTable& symbol_table_;
};

struct HeterogeneousStatNameHash {
  // Specifying is_transparent indicates to the library infrastructure that
  // type-conversions should not be applied when calling find(), but instead
  // pass the actual types of the contained and searched-for objects directly to
  // these functors. See
  // https://en.cppreference.com/w/cpp/utility/functional/less_void for an
  // official reference, and https://abseil.io/tips/144 for a description of
  // using it in the context of absl.
  using is_transparent = void;

  size_t operator()(StatName a) const { return a.hash(); }
  size_t operator()(const StatNameStorage& a) const { return a.statName().hash(); }
};

struct HeterogeneousStatNameEqual {
  // See description for HeterogeneousStatNameHash::is_transparent.
  using is_transparent = void;

  size_t operator()(StatName a, StatName b) const { return a == b; }
  size_t operator()(const StatNameStorage& a, const StatNameStorage& b) const {
    return a.statName() == b.statName();
  }
  size_t operator()(StatName a, const StatNameStorage& b) const { return a == b.statName(); }
  size_t operator()(const StatNameStorage& a, StatName b) const { return a.statName() == b; }
};

// Encapsulates a set<StatNameStorage>. We use containment here rather than a
// 'using' alias because we need to ensure that when the set is destructed,
// StatNameStorage::free(symbol_table) is called on each entry. It is a little
// easier at the call-sites in thread_local_store.cc to implement this an
// explicit free() method, analogous to StatNameStorage::free(), compared to
// storing a SymbolTable reference in the class and doing the free in the
// destructor, like StatNameManagedStorage.
class StatNameStorageSet {
public:
  using HashSet =
      absl::flat_hash_set<StatNameStorage, HeterogeneousStatNameHash, HeterogeneousStatNameEqual>;
  using iterator = HashSet::iterator;

  ~StatNameStorageSet();

  /**
   * Releases all symbols held in this set. Must be called prior to destruction.
   *
   * @param symbol_table The symbol table that owns the symbols.
   */
  void free(SymbolTable& symbol_table);

  /**
   * @param storage The StatNameStorage to add to the set.
   */
  std::pair<HashSet::iterator, bool> insert(StatNameStorage&& storage) {
    return hash_set_.insert(std::move(storage));
  }

  /**
   * @param stat_name The stat_name to find.
   * @return the iterator pointing to the stat_name, or end() if not found.
   */
  iterator find(StatName stat_name) { return hash_set_.find(stat_name); }

  /**
   * @return the end-marker.
   */
  iterator end() { return hash_set_.end(); }

  /**
   * @param set the storage set to swap with.
   */
  void swap(StatNameStorageSet& set) { hash_set_.swap(set.hash_set_); }

  /**
   * @return the number of elements in the set.
   */
  size_t size() const { return hash_set_.size(); }

private:
  HashSet hash_set_;
};

} // namespace Stats
} // namespace Envoy
