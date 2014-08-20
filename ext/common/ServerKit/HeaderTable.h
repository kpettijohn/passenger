/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SERVER_KIT_HEADER_TABLE_H_
#define _PASSENGER_SERVER_KIT_HEADER_TABLE_H_

#include <boost/cstdint.hpp>
#include <cstring>
#include <cassert>

#include <DataStructures/LString.h>
#include <DataStructures/HashedStaticString.h>
#include <StaticString.h>

namespace Passenger {
namespace ServerKit {

using namespace std;


struct Header {
	LString key;
	LString val;
	boost::uint32_t hash;
};


/**
 * A hash table, optimized for storing HTTP headers. It assumes the following workload:
 *
 *  * Inserts happen in bulk, soon after hash table creation or clearing.
 *  * Once the bulk insertion phase is over, lookups are frequent, but modifications
 *    are not.
 *  * The hash table does not contain a lot of elements. Maybe 35 or so.
 *
 * The hash table uses open addressing and linear probing for cache friendliness. It
 * supports keys that are non-contigunous in memory, through the use of LString.
 *
 * It supports at most 2^16-1 keys.
 *
 * The hash table automatically doubles in size when it becomes 75% full.
 * The hash table never shrinks in size, even after clear(), unless you explicitly call
 * compact(). This allows you to reuse hash table memory over multiple requests.
 *
 * This implementation is based on https://github.com/preshing/CompareIntegerMaps.
 * See also http://preshing.com/20130107/this-hash-table-is-faster-than-a-judy-array
 */
class HeaderTable {
public:
	#define PHT_FIRST_CELL(hash) (m_cells + ((hash) & (m_arraySize - 1)))
	#define PHT_CIRCULAR_NEXT(c) ((c) + 1 != m_cells + m_arraySize ? (c) + 1 : m_cells)
	#define PHT_CIRCULAR_OFFSET(a, b) ((b) >= (a) ? (b) - (a) : m_arraySize + (b) - (a))

	static const unsigned int MAX_KEY_LENGTH = 65535;
	static const unsigned int DEFAULT_SIZE = 64;

	struct Cell {
		Header *header;
	};

private:
	Cell *m_cells;
	boost::uint16_t m_arraySize;
	boost::uint16_t m_population;

	bool shouldRepopulateOnInsert() const {
		return (m_population + 1) * 4 >= m_arraySize * 3;
	}

	OXT_FORCE_INLINE
	bool cellIsEmpty(const Cell * const cell) {
		return cell->header == NULL;
	}

	static boost::uint32_t upper_power_of_two(boost::uint32_t v) {
		v--;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		v++;
		return v;
	}

	void repopulate(unsigned int desiredSize) {
		assert((desiredSize & (desiredSize - 1)) == 0);   // Must be a power of 2
		assert(m_population * 4  <= desiredSize * 3);

		// Get start/end pointers of old array
		Cell *oldCells = m_cells;
		Cell *end = m_cells + m_arraySize;

		// Allocate new array
		m_arraySize = desiredSize;
		m_cells = new Cell[m_arraySize];
		memset(m_cells, 0, sizeof(Cell) * m_arraySize);

		if (oldCells == NULL) {
			return;
		}

		// Iterate through old array
		for (Cell *oldCell = oldCells; oldCell != end; oldCell++) {
			if (!cellIsEmpty(oldCell)) {
				// Insert this element into new array
				Cell *newCell = PHT_FIRST_CELL(oldCell->header->hash);
				while (true) {
					if (cellIsEmpty(newCell)) {
						// Insert here
						*newCell = *oldCell;
						break;
					} else {
						newCell = PHT_CIRCULAR_NEXT(newCell);
					}
				}
			}
		}

		// Delete old array
		delete[] oldCells;
	}

	void copyFrom(const HeaderTable &other) {
		m_arraySize  = other.m_arraySize;
		m_population = other.m_population;
		m_cells      = new Cell[other.m_arraySize];
		memcpy(m_cells, other.m_cells, other.m_arraySize * sizeof(Cell));
	}

public:
	HeaderTable(unsigned int initialSize = DEFAULT_SIZE)
	{
		init(initialSize);
	}

	HeaderTable(const HeaderTable &other) {
		copyFrom(other);
	}

	~HeaderTable() {
		delete[] m_cells;
	}

	HeaderTable &operator=(const HeaderTable &other) {
		delete[] m_cells;
		copyFrom(other);
		return *this;
	}

	void init(unsigned int initialSize) {
		assert((initialSize & (initialSize - 1)) == 0);   // Must be a power of 2

		m_arraySize = initialSize;
		if (initialSize == 0) {
			m_cells = NULL;
		} else {
			m_cells = new Cell[m_arraySize];
			memset(m_cells, 0, sizeof(Cell) * m_arraySize);
		}
		m_population = 0;
	}

	Cell *lookupCell(const HashedStaticString &key) {
		assert(!key.empty());
		assert(key.size() < MAX_KEY_LENGTH);

		if (m_cells == NULL) {
			return NULL;
		}

		Cell *cell = PHT_FIRST_CELL(key.hash());
		while (true) {
			if (cellIsEmpty(cell)) {
				// Empty cell found.
				return NULL;
			} else if (psg_lstr_cmp(&cell->header->key, key)) {
				// Non-empty cell found.
				return cell;
			} else {
				// Keep probing.
				cell = PHT_CIRCULAR_NEXT(cell);
			}
		}
	}

	const LString *lookup(const HashedStaticString &key) {
		const Cell * const cell = lookupCell(key);
		if (cell != NULL) {
			return &cell->header->val;
		} else {
			return NULL;
		}
	}

	/** header must stay alive */
	void insert(Header *header, bool overwrite = true) {
		assert(header->key.size < MAX_KEY_LENGTH);

		if (m_cells == NULL) {
			repopulate(DEFAULT_SIZE);
		}

		while (true) {
			Cell *cell = PHT_FIRST_CELL(header->hash);
			while (true) {
				if (cellIsEmpty(cell)) {
					// Cell is empty. Insert here.
					if (shouldRepopulateOnInsert()) {
						// Time to resize
						repopulate(m_arraySize * 2);
						break;
					}
					m_population++;

					cell->header = header;
					return;
				} else if (psg_lstr_cmp(&cell->header->key, &header->key)) {
					// Cell matches.
					if (overwrite) {
						psg_lstr_deinit(&cell->header->key);
						psg_lstr_deinit(&cell->header->val);
						cell->header = header;
					}
					return;
				} else {
					cell = PHT_CIRCULAR_NEXT(cell);
				}
			}
		}
	}

	void erase(Cell *cell) {
		assert(cell >= m_cells && cell - m_cells < m_arraySize);
		assert(!cellIsEmpty(cell));

		// Remove this cell by shuffling neighboring cells so there are no gaps in anyone's probe chain
		Cell *neighbor = PHT_CIRCULAR_NEXT(cell);
		while (true) {
			if (cellIsEmpty(neighbor)) {
				// There's nobody to swap with. Go ahead and clear this cell, then return.
				cell->header = NULL;
				m_population--;
				return;
			}

			Cell *ideal = PHT_FIRST_CELL(neighbor->header->hash);
			if (PHT_CIRCULAR_OFFSET(ideal, cell) < PHT_CIRCULAR_OFFSET(ideal, neighbor)) {
				// Swap with neighbor, then make neighbor the new cell to remove.
				*cell = *neighbor;
				cell = neighbor;
			}
			neighbor = PHT_CIRCULAR_NEXT(neighbor);
		}
	}

	void erase(const HashedStaticString &key) {
		Cell *cell = lookupCell(key);
		if (cell != NULL) {
			erase(cell);
		}
	}

	/** Does not resize the array. */
	void clear() {
		if (m_cells != NULL && m_population != 0) {
			memset(m_cells, 0, sizeof(Cell) * m_arraySize);
		}
		m_population = 0;
	}

	void freeMemory() {
		delete[] m_cells;
		m_cells = NULL;
		m_arraySize  = 0;
		m_population = 0;
	}

	void compact() {
		repopulate(upper_power_of_two((m_population * 4 + 3) / 3));
	}

	unsigned int size() const {
		return m_population;
	}

	unsigned int arraySize() const {
		return m_arraySize;
	}


	friend class Iterator;
	class Iterator {
	private:
		HeaderTable *m_table;
		Cell *m_cur;

	public:
		Iterator(HeaderTable &table)
			: m_table(&table)
		{
			if (m_table->m_cells != NULL) {
				m_cur = &m_table->m_cells[0];
				if (m_table->cellIsEmpty(m_cur)) {
					next();
				}
			} else {
				m_cur = NULL;
			}
		}

		Cell *next() {
			if (m_cur == NULL) {
				// Already finished.
				return NULL;
			}

			Cell *end = m_table->m_cells + m_table->m_arraySize;
			while (++m_cur != end) {
				if (!m_table->cellIsEmpty(m_cur)) {
					return m_cur;
				}
			}

			// Finished
			return m_cur = NULL;
		}

		inline Cell *operator*() const {
			return m_cur;
		}

		inline Cell *operator->() const {
			return m_cur;
		}
	};
};


} // namespace ServerKit
} // namespace Passenger

#endif /* _PASSENGER_SERVER_KIT_HEADER_TABLE_H_ */
