#ifndef _RAC_H_
#define _RAC_H_ 1

#include <stdio.h>
#include <stdint.h>
#include <assert.h>


/* RAC configuration for 40-bit RAC */
class RacConfig40
{
public:
    typedef uint64_t data_t;
    static const int MAX_RANGE_BITS = 40;
    static const int MIN_RANGE_BITS = (MAX_RANGE_BITS-8);
    static const data_t MIN_RANGE = (1ULL << (data_t)(MIN_RANGE_BITS));
    static const data_t BASE_RANGE = (1ULL << MAX_RANGE_BITS);
};

/* RAC configuration for 24-bit RAC */
class RacConfig24
{
public:
    typedef uint32_t data_t;
    static const data_t MAX_RANGE_BITS = 24;
    static const data_t MIN_RANGE_BITS = (MAX_RANGE_BITS-8);
    static const data_t MIN_RANGE = (1ULL << MIN_RANGE_BITS);
    static const data_t BASE_RANGE = (1ULL << MAX_RANGE_BITS);
};

template <typename Config, typename IO> class RacInput
{
public:
    typedef typename Config::data_t rac_t;
protected:
    IO io;
private:
    rac_t range;
    rac_t low;
private:
    void inline input() {
        while (range <= Config::MIN_RANGE) {
            low <<= 8;
            range <<= 8;
            low |= io.read();
        }
    }
    bool inline get(rac_t chance) {
        assert(chance > 0);
        assert(chance < range);
        if (low >= range - chance) {
            low -= range - chance;
            range = chance;
            input();
            return 1;
        } else {
            range -= chance;
            input();
            return 0;
        }
    }
public:
    RacInput(IO ioin) : io(ioin), range(Config::BASE_RANGE), low(0) {
        rac_t r = Config::BASE_RANGE;
        while (r > 1) {
            low <<= 8;
            low |= io.read();
            r >>= 8;
        }
    }

    bool inline read(int num, int denom) {
        assert(num>=0);
        assert(num<denom);
        assert(denom>0);
        return get(((uint64_t)range * num + denom / 2) / denom);
    }

    bool inline read(uint16_t b16) {
        assert(b16>0);
        return get(((uint64_t)range * b16 + 0x8000) >> 16);
    }

    bool inline read() {
        return get(range/2);
    }
};

template <class Config, class IO> class RacOutput
{
public:
    typedef typename Config::data_t rac_t;
protected:
    IO io;
private:
    rac_t range;
    rac_t low;
    int delayed_byte;
    int delayed_count;
    void inline output() {
        while (range <= Config::MIN_RANGE) {
            int byte = low >> Config::MIN_RANGE_BITS;
            if (delayed_byte < 0) { // first generated byte
                delayed_byte = byte;
            } else if (((low + range) >> 8) < Config::MIN_RANGE) { // definitely no overflow
                io.write(delayed_byte);
                while (delayed_count) {
                    io.write(0xFF);
                    delayed_count--;
                }
                delayed_byte = byte;
            } else if ((low >> 8) >= Config::MIN_RANGE) { // definitely overflow
                io.write(delayed_byte + 1);
                while (delayed_count) {
                    io.write(0);
                    delayed_count--;
                }
                delayed_byte = byte & 0xFF;
            } else {
                delayed_count++;
            }
            low = (low & (Config::MIN_RANGE - 1)) << 8;
            range <<= 8;
        }
    }
    void inline put(rac_t chance, bool bit) {
        assert(chance > 0);
        assert(chance < range);
        if (bit) {
            low += range - chance;
            range = chance;
        } else {
            range -= chance;
        }
        output();
    }
public:
    RacOutput(IO ioin) : io(ioin), range(Config::BASE_RANGE), low(0), delayed_byte(-1), delayed_count(0) { }

    void inline write(int num, int denom, bool bit) {
        assert(num>=0);
        assert(num<denom);
        assert(denom>1);
        put(((uint64_t)range * num + denom / 2) / denom, bit);
    }

    void inline write(uint16_t b16, bool bit) {
        assert(b16>0);
        put(((uint64_t)range * b16 + 0x8000) >> 16, bit);
    }

    void inline write(bool bit) {
        put(range / 2, bit);
    }

    void inline flush() {
        low += (Config::MIN_RANGE - 1);
        range = Config::MIN_RANGE - 1;
        output();
        range = Config::MIN_RANGE - 1;
        output();
        io.flush();
    }
};


class RacDummy
{
public:
    void inline write(int num, int denom, bool bit) {
        assert(num>=0);
        assert(num<denom);
        assert(denom>1);
    }

    void inline write(uint16_t b16, bool bit) {
        assert(b16>0);
    }

    void inline write(bool bit) { }
    void inline flush() { }
};


class RacFileIO
{
private:
    FILE *file;
public:
    RacFileIO(FILE* fil) : file(fil) { }
    int read() {
        int r = fgetc(file);
        if (r < 0) return 0;
        return r;
    }
    void write(int byte) {
        fputc(byte, file);
    }
    void flush() {
    }
};

class RacInput40 : public RacInput<RacConfig40, RacFileIO>
{
public:
    RacInput40(FILE *file) : RacInput<RacConfig40, RacFileIO>(RacFileIO(file)) { }
};

class RacOutput40 : public RacOutput<RacConfig40, RacFileIO>
{
public:
    RacOutput40(FILE *file) : RacOutput<RacConfig40, RacFileIO>(RacFileIO(file)) { }
};

class RacInput24 : public RacInput<RacConfig24, RacFileIO>
{
public:
    RacInput24(FILE *file) : RacInput<RacConfig24, RacFileIO>(RacFileIO(file)) { }
};

class RacOutput24 : public RacOutput<RacConfig24, RacFileIO>
{
public:
    RacOutput24(FILE *file) : RacOutput<RacConfig24, RacFileIO>(RacFileIO(file)) { }
};

#endif
