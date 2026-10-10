// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/scummsys.h"

namespace Common {

// A file in memory. Reading beyond its end gives zeros and sets eos().
class SeekableReadStream {
public:
	SeekableReadStream(const byte *data, size_t size) : _data(data), _size(size), _pos(0), _eos(false) {}
	size_t size() const { return _size; }
	size_t pos() const { return _pos; }
	bool eos() const { return _eos; }
	bool err() const { return false; }
	uint32 read(void *to, uint32 count) {
		uint32 can = count;
		if (_pos + count > _size) {
			can = _pos < _size ? (uint32)(_size - _pos) : 0;
			_eos = true;
			memset((byte *)to + can, 0, count - can);
		}
		memcpy(to, _data + _pos, can);
		_pos += count;
		return can;
	}
	void skip(uint32 count) {
		_pos += count;
		if (_pos > _size) _eos = true;
	}
	byte readByte() { byte b; read(&b, 1); return b; }
	uint16 readUint16BE() { byte b[2]; read(b, 2); return (uint16)((b[0] << 8) | b[1]); }
	int16 readSint16BE() { return (int16)readUint16BE(); }
	uint32 readUint32BE() { byte b[4]; read(b, 4); return ((uint32)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }

private:
	const byte *_data;
	size_t _size, _pos;
	bool _eos;
};

}
