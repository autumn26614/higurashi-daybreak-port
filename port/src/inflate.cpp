#include "inflate.h"

#include <cstring>

namespace db {
namespace {

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool overrun = false;

    int bits(int need) {
        while (bitcnt < need) {
            if (pos >= n) {
                overrun = true;
                return 0;
            }
            bitbuf |= uint32_t(p[pos++]) << bitcnt;
            bitcnt += 8;
        }
        int v = int(bitbuf & ((1u << need) - 1));
        bitbuf >>= need;
        bitcnt -= need;
        return v;
    }
};

struct Huffman {
    short count[16];
    short symbol[288];
};

// Canonical Huffman table build (as in zlib's puff.c). Returns <0 on over-subscription.
int build(Huffman& h, const short* length, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; s++) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = short(offs[len] + h.count[len]);
    for (int s = 0; s < n; s++)
        if (length[s] != 0) h.symbol[offs[length[s]]++] = short(s);
    return left;
}

int decode(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= br.bits(1);
        if (br.overrun) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                             6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(BitReader& br, std::vector<uint8_t>& win, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int sym = decode(br, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            win.push_back(uint8_t(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + br.bits(kLenExtra[sym]);
            int dsym = decode(br, distcode);
            if (dsym < 0 || dsym >= 30) return false;
            size_t dist = size_t(kDistBase[dsym] + br.bits(kDistExtra[dsym]));
            if (br.overrun || dist > win.size()) return false;
            size_t from = win.size() - dist;
            for (int i = 0; i < len; i++) win.push_back(win[from + size_t(i)]);
        }
    }
}

}  // namespace

bool inflate_raw(const uint8_t* in, size_t in_len, const std::vector<uint8_t>& dict,
                 std::vector<uint8_t>& out, std::string* err) {
    BitReader br{in, in_len};
    std::vector<uint8_t> win(dict);  // window = dictionary + output so far
    const size_t start = win.size();
    int last;
    do {
        last = br.bits(1);
        int type = br.bits(2);
        if (br.overrun) break;
        if (type == 0) {  // stored
            br.bitbuf = 0;
            br.bitcnt = 0;
            if (br.pos + 4 > in_len) break;
            unsigned len = in[br.pos] | (in[br.pos + 1] << 8);
            br.pos += 4;
            if (br.pos + len > in_len) break;
            win.insert(win.end(), in + br.pos, in + br.pos + len);
            br.pos += len;
        } else if (type == 1) {  // fixed Huffman
            static Huffman lencode, distcode;
            static bool init = false;
            if (!init) {
                short lengths[288];
                int s = 0;
                for (; s < 144; s++) lengths[s] = 8;
                for (; s < 256; s++) lengths[s] = 9;
                for (; s < 280; s++) lengths[s] = 7;
                for (; s < 288; s++) lengths[s] = 8;
                build(lencode, lengths, 288);
                for (s = 0; s < 30; s++) lengths[s] = 5;
                build(distcode, lengths, 30);
                init = true;
            }
            if (!codes(br, win, lencode, distcode)) {
                *err = "inflate: bad fixed block";
                return false;
            }
        } else if (type == 2) {  // dynamic Huffman
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = br.bits(5) + 257, ndist = br.bits(5) + 1, ncode = br.bits(4) + 4;
            if (nlen > 286 || ndist > 30) {
                *err = "inflate: bad counts";
                return false;
            }
            short lengths[320] = {0};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = short(br.bits(3));
            Huffman lencode, distcode;
            if (build(lencode, lengths, 19) != 0) {
                *err = "inflate: bad code lengths";
                return false;
            }
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode(br, lencode);
                if (sym < 0) {
                    *err = "inflate: bad code-length symbol";
                    return false;
                }
                if (sym < 16) {
                    lengths[idx++] = short(sym);
                } else {
                    short len = 0;
                    int rep;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        len = lengths[idx - 1];
                        rep = 3 + br.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (idx + rep > nlen + ndist) {
                        *err = "inflate: too many lengths";
                        return false;
                    }
                    while (rep--) lengths[idx++] = len;
                }
            }
            // Over-subscribed tables are corrupt; incomplete ones are tolerated (as zlib does
            // for single-code distance trees).
            if (lengths[256] == 0 || build(lencode, lengths, nlen) < 0 ||
                build(distcode, lengths + nlen, ndist) < 0) {
                *err = "inflate: bad literal/distance code set";
                return false;
            }
            if (!codes(br, win, lencode, distcode)) {
                *err = "inflate: bad dynamic block";
                return false;
            }
        } else {
            *err = "inflate: invalid block type";
            return false;
        }
    } while (!last);
    if (br.overrun) {
        *err = "inflate: input overrun";
        return false;
    }
    out.insert(out.end(), win.begin() + long(start), win.end());
    return true;
}

}  // namespace db
