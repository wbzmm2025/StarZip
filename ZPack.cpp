#include "ZPack.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace {

constexpr int kMagicSize = 4;
constexpr int kHeaderSize = 4 + 1 + 8 + 4;
constexpr int kMinBlock = 64 * 1024;
constexpr int kMaxBlock = 1024 * 1024;
constexpr int kWindow = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
constexpr int kMaxChain = 96;
constexpr int kEndSymbol = 256;
constexpr int kLengthStart = 257;
constexpr int kDistanceStart = 286;
constexpr int kMaxCodeLength = 60;

const int kLengthBase[29] = {3,   4,   5,   6,   7,    8,    9,    10,   11,   13,
                             15,  17,  19,  23,  27,   31,   35,   43,   51,   59,
                             67,  83,  99,  115, 131,  163,  195,  227,  258};
const int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistanceBase[30] = {1,    2,    3,    4,    5,     7,     9,     13,
                               17,   25,   33,   49,   65,    97,    129,   193,
                               257,  385,  513,  769,  1025,  1537,  2049,  3073,
                               4097, 6145, 8193, 12289, 16385, 24577};
const int kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

quint32 crcTable[256];
bool crcTableReady = false;

void ensureCrcTable()
{
    if (crcTableReady)
        return;
    for (quint32 i = 0; i < 256; ++i) {
        quint32 c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crcTable[i] = c;
    }
    crcTableReady = true;
}

quint32 crc32Bytes(const char *data, int size)
{
    ensureCrcTable();
    quint32 crc = 0xffffffffu;
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    for (int i = 0; i < size; ++i)
        crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

quint32 crc32(const QByteArray &data)
{
    return crc32Bytes(data.constData(), data.size());
}

void appendLE16(QByteArray *out, quint32 value)
{
    out->append(char(value & 0xff));
    out->append(char((value >> 8) & 0xff));
}

void appendLE32(QByteArray *out, quint32 value)
{
    out->append(char(value & 0xff));
    out->append(char((value >> 8) & 0xff));
    out->append(char((value >> 16) & 0xff));
    out->append(char((value >> 24) & 0xff));
}

void appendLE64(QByteArray *out, quint64 value)
{
    for (int i = 0; i < 8; ++i)
        out->append(char((value >> (i * 8)) & 0xff));
}

quint16 readLE16(const QByteArray &data, int offset, bool *ok)
{
    if (offset < 0 || offset + 2 > data.size()) {
        if (ok)
            *ok = false;
        return 0;
    }
    return quint16(quint8(data.at(offset))) | (quint16(quint8(data.at(offset + 1))) << 8);
}

quint32 readLE32(const QByteArray &data, int offset, bool *ok)
{
    if (offset < 0 || offset + 4 > data.size()) {
        if (ok)
            *ok = false;
        return 0;
    }
    return quint32(quint8(data.at(offset)))
           | (quint32(quint8(data.at(offset + 1))) << 8)
           | (quint32(quint8(data.at(offset + 2))) << 16)
           | (quint32(quint8(data.at(offset + 3))) << 24);
}

quint64 readLE64(const QByteArray &data, int offset, bool *ok)
{
    if (offset < 0 || offset + 8 > data.size()) {
        if (ok)
            *ok = false;
        return 0;
    }
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value |= quint64(quint8(data.at(offset + i))) << (i * 8);
    return value;
}

// ---------------- 位流 ----------------

class BitWriter
{
public:
    void writeBit(int bit)
    {
        m_current = quint8(m_current | ((bit & 1) << m_bitCount));
        ++m_bitCount;
        if (m_bitCount == 8) {
            m_data.append(char(m_current));
            m_current = 0;
            m_bitCount = 0;
        }
    }

    void writeBits(quint64 value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
            writeBit(int((value >> i) & 1));
    }

    void finish()
    {
        if (m_bitCount > 0) {
            m_data.append(char(m_current));
            m_current = 0;
        }
    }

    int pendingBits() const { return m_bitCount; }
    const QByteArray &data() const { return m_data; }
    QByteArray &data() { return m_data; }

private:
    QByteArray m_data;
    quint8 m_current = 0;
    int m_bitCount = 0;
};

class BitReader
{
public:
    BitReader(const QByteArray &data, int bitLimit)
        : m_data(data)
        , m_bitLimit(bitLimit < 0 ? data.size() * 8 : bitLimit)
    {
    }

    bool readBit(int *bit)
    {
        if (m_bitPos >= m_bitLimit || m_bitPos >= m_data.size() * 8)
            return false;
        const int byte = quint8(m_data.at(m_bitPos >> 3));
        *bit = (byte >> (m_bitPos & 7)) & 1;
        ++m_bitPos;
        return true;
    }

    bool readBits(int count, quint32 *value)
    {
        quint32 result = 0;
        for (int i = 0; i < count; ++i) {
            int bit = 0;
            if (!readBit(&bit))
                return false;
            result = (result << 1) | quint32(bit);
        }
        *value = result;
        return true;
    }

private:
    const QByteArray &m_data;
    int m_bitPos = 0;
    int m_bitLimit = 0;
};

// ---------------- 规范 Huffman ----------------

bool buildCodeLengths(const QVector<quint64> &freq, QVector<quint8> *lengths)
{
    const int n = freq.size();
    lengths->fill(0, n);

    QVector<int> present;
    present.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (freq.at(i) > 0)
            present.append(i);
    }
    if (present.isEmpty())
        return true;
    if (present.size() == 1) {
        (*lengths)[present.first()] = 1;
        return true;
    }

    struct Node
    {
        quint64 weight = 0;
        int left = -1;
        int right = -1;
        int symbol = -1;
    };
    QVector<Node> nodes;
    nodes.reserve(present.size() * 2);

    using Item = std::pair<quint64, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    for (int symbol : present) {
        Node node;
        node.weight = freq.at(symbol);
        node.symbol = symbol;
        nodes.append(node);
        queue.push({node.weight, nodes.size() - 1});
    }

    while (queue.size() > 1) {
        const Item first = queue.top();
        queue.pop();
        const Item second = queue.top();
        queue.pop();
        Node parent;
        parent.weight = first.first + second.first;
        parent.left = first.second;
        parent.right = second.second;
        nodes.append(parent);
        queue.push({parent.weight, nodes.size() - 1});
    }

    struct Walk
    {
        int node;
        int depth;
    };
    std::vector<Walk> stack;
    stack.push_back({queue.top().second, 0});
    int maxDepth = 0;
    while (!stack.empty()) {
        const Walk walk = stack.back();
        stack.pop_back();
        const Node &node = nodes.at(walk.node);
        if (node.symbol >= 0) {
            const int depth = qMax(1, walk.depth);
            (*lengths)[node.symbol] = quint8(qMin(depth, 255));
            maxDepth = qMax(maxDepth, depth);
        } else {
            stack.push_back({node.left, walk.depth + 1});
            stack.push_back({node.right, walk.depth + 1});
        }
    }

    // 极端偏斜的数据可能产生过深的码字。使用等长码回退，保持前缀码有效。
    if (maxDepth > kMaxCodeLength) {
        int bits = 1;
        while ((quint64(1) << bits) < quint64(present.size()) && bits < kMaxCodeLength)
            ++bits;
        for (int symbol : present)
            (*lengths)[symbol] = quint8(bits);
    }
    return true;
}

bool buildCanonical(const QVector<quint8> &lengths, QVector<quint64> *codes,
                    QVector<quint64> *firstCode = nullptr, QVector<int> *firstSymbol = nullptr,
                    QVector<quint16> *sortedSymbols = nullptr, int *maxLength = nullptr,
                    QVector<int> *lengthCounts = nullptr)
{
    int maxLen = 0;
    for (quint8 length : lengths)
        maxLen = qMax(maxLen, int(length));
    if (maxLen > kMaxCodeLength)
        return false;

    QVector<int> count(maxLen + 1, 0);
    for (quint8 length : lengths) {
        if (length > 0)
            ++count[int(length)];
    }

    // 拒绝过满的码表，避免损坏文件构造出无效前缀码。
    quint64 kraft = 0;
    for (int length = 1; length <= maxLen; ++length)
        kraft += quint64(count[length]) << (maxLen - length);
    if (maxLen > 0 && kraft > (quint64(1) << maxLen))
        return false;

    QVector<quint64> first(maxLen + 2, 0);
    for (int length = 1; length <= maxLen; ++length)
        first[length] = (first[length - 1] + quint64(count[length - 1])) << 1;

    QVector<quint16> sorted;
    QVector<int> offsets(maxLen + 2, 0);
    for (int symbol = 0; symbol < lengths.size(); ++symbol) {
        const int length = lengths.at(symbol);
        if (length > 0)
            sorted.append(quint16(symbol));
    }
    std::sort(sorted.begin(), sorted.end(), [&lengths](quint16 left, quint16 right) {
        const int leftLength = lengths.at(left);
        const int rightLength = lengths.at(right);
        if (leftLength != rightLength)
            return leftLength < rightLength;
        return left < right;
    });

    QVector<quint64> next = first;
    codes->fill(0, lengths.size());
    for (int symbol = 0; symbol < lengths.size(); ++symbol) {
        const int length = lengths.at(symbol);
        if (length > 0)
            (*codes)[symbol] = next[length]++;
    }

    if (firstCode)
        *firstCode = first;
    if (maxLength)
        *maxLength = maxLen;
    if (lengthCounts)
        *lengthCounts = count;
    if (firstSymbol) {
        firstSymbol->fill(0, maxLen + 2);
        for (int i = 0; i < sorted.size(); ++i)
            offsets[lengths.at(sorted.at(i))]++;
        int running = 0;
        for (int length = 1; length <= maxLen; ++length) {
            (*firstSymbol)[length] = running;
            running += offsets[length];
        }
    }
    if (sortedSymbols)
        *sortedSymbols = sorted;
    return true;
}

int decodeSymbol(BitReader &reader, const QVector<quint64> &firstCode,
                 const QVector<int> &lengthCounts, const QVector<int> &firstSymbol,
                 const QVector<quint16> &sortedSymbols, int maxLength)
{
    quint64 code = 0;
    for (int length = 1; length <= maxLength; ++length) {
        int bit = 0;
        if (!reader.readBit(&bit))
            return -1;
        code = (code << 1) | quint64(bit);
        const int count = lengthCounts.at(length);
        if (count > 0 && code >= firstCode.at(length)
            && code < firstCode.at(length) + quint64(count)) {
            const int index = firstSymbol.at(length) + int(code - firstCode.at(length));
            if (index >= 0 && index < sortedSymbols.size())
                return sortedSymbols.at(index);
            return -1;
        }
    }
    return -1;
}

// ---------------- LZ77 ----------------

struct Token
{
    bool match = false;
    quint8 literal = 0;
    quint16 length = 0;
    quint16 distance = 0;
};

int hash3(const QByteArray &data, int pos)
{
    const quint32 a = quint8(data.at(pos));
    const quint32 b = quint8(data.at(pos + 1));
    const quint32 c = quint8(data.at(pos + 2));
    return int(((a << 10) ^ (b << 5) ^ c) & (kHashSize - 1));
}

void insertPosition(const QByteArray &data, int pos, std::vector<int> *head,
                    std::vector<int> *prev)
{
    if (pos + 2 >= data.size())
        return;
    const int h = hash3(data, pos);
    (*prev)[pos] = (*head)[h];
    (*head)[h] = pos;
}

void findMatch(const QByteArray &data, int pos, const std::vector<int> &head,
               const std::vector<int> &prev, int *bestLength, int *bestDistance)
{
    *bestLength = 0;
    *bestDistance = 0;
    if (pos + kMinMatch > data.size())
        return;

    const int h = hash3(data, pos);
    int candidate = head[h];
    const int maxLength = qMin(kMaxMatch, data.size() - pos);
    int chain = 0;
    const char *source = data.constData();

    while (candidate >= 0 && pos - candidate <= kWindow && chain < kMaxChain) {
        ++chain;
        int length = 0;
        while (length < maxLength
               && source[candidate + length] == source[pos + length])
            ++length;
        if (length > *bestLength) {
            *bestLength = length;
            *bestDistance = pos - candidate;
            if (length == maxLength)
                break;
        }
        candidate = prev[candidate];
    }

    if (*bestLength < kMinMatch) {
        *bestLength = 0;
        *bestDistance = 0;
    }
}

QVector<Token> tokenize(const QByteArray &data, const std::atomic_bool *cancel, bool *canceled)
{
    QVector<Token> tokens;
    tokens.reserve(qMax(16, data.size() / 3));
    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(data.size(), -1);

    int pos = 0;
    while (pos < data.size()) {
        if (cancel && cancel->load()) {
            *canceled = true;
            return tokens;
        }

        int length = 0;
        int distance = 0;
        findMatch(data, pos, head, prev, &length, &distance);
        insertPosition(data, pos, &head, &prev);

        // 一步惰性匹配：下一个位置明显更好时先输出字面量。
        if (length >= kMinMatch && length < kMaxMatch && pos + 1 < data.size()) {
            int nextLength = 0;
            int nextDistance = 0;
            findMatch(data, pos + 1, head, prev, &nextLength, &nextDistance);
            if (nextLength > length + 1) {
                Token literal;
                literal.literal = quint8(data.at(pos));
                tokens.append(literal);
                ++pos;
                continue;
            }
        }

        Token token;
        if (length >= kMinMatch) {
            token.match = true;
            token.length = quint16(length);
            token.distance = quint16(distance);
            tokens.append(token);
            for (int i = 1; i < length; ++i)
                insertPosition(data, pos + i, &head, &prev);
            pos += length;
        } else {
            token.literal = quint8(data.at(pos));
            tokens.append(token);
            ++pos;
        }
    }
    return tokens;
}

int lengthCodeFor(int length, int *extra, int *extraBits)
{
    for (int i = 0; i < 29; ++i) {
        const int span = 1 << kLengthExtra[i];
        if (length >= kLengthBase[i] && (length < kLengthBase[i] + span || i == 28)) {
            *extra = length - kLengthBase[i];
            *extraBits = kLengthExtra[i];
            return i;
        }
    }
    *extra = 0;
    *extraBits = 0;
    return 0;
}

int distanceCodeFor(int distance, int *extra, int *extraBits)
{
    for (int i = 0; i < 30; ++i) {
        const int span = 1 << kDistanceExtra[i];
        if (distance >= kDistanceBase[i]
            && (distance < kDistanceBase[i] + span || i == 29)) {
            *extra = distance - kDistanceBase[i];
            *extraBits = kDistanceExtra[i];
            return i;
        }
    }
    *extra = 0;
    *extraBits = 0;
    return 0;
}

// ---------------- 块编码 ----------------

struct EncodedBlock
{
    quint8 type = 0;
    QByteArray data;
    QVector<quint8> lengths;
    quint8 validBits = 0;
    quint32 crc = 0;
};

bool encodeTokens(const QVector<Token> &tokens, int tableSize, QVector<quint8> *lengths,
                  QByteArray *payload, quint8 *validBits)
{
    QVector<quint64> freq(tableSize, 0);
    for (const Token &token : tokens) {
        if (token.match) {
            int extra = 0;
            int extraBits = 0;
            const int lc = lengthCodeFor(token.length, &extra, &extraBits);
            const int dc = distanceCodeFor(token.distance, &extra, &extraBits);
            ++freq[kLengthStart + lc];
            ++freq[kDistanceStart + dc];
        } else {
            ++freq[token.literal];
        }
    }
    ++freq[kEndSymbol];

    QVector<quint64> codes;
    if (!buildCodeLengths(freq, lengths) || !buildCanonical(*lengths, &codes))
        return false;

    BitWriter writer;
    for (const Token &token : tokens) {
        if (token.match) {
            int lengthExtra = 0;
            int lengthExtraBits = 0;
            const int lc = lengthCodeFor(token.length, &lengthExtra, &lengthExtraBits);
            writer.writeBits(codes.at(kLengthStart + lc), lengths->at(kLengthStart + lc));
            if (lengthExtraBits > 0)
                writer.writeBits(quint64(lengthExtra), lengthExtraBits);

            int distanceExtra = 0;
            int distanceExtraBits = 0;
            const int dc =
                distanceCodeFor(token.distance, &distanceExtra, &distanceExtraBits);
            writer.writeBits(codes.at(kDistanceStart + dc),
                             lengths->at(kDistanceStart + dc));
            if (distanceExtraBits > 0)
                writer.writeBits(quint64(distanceExtra), distanceExtraBits);
        } else {
            writer.writeBits(codes.at(token.literal), lengths->at(token.literal));
        }
    }
    writer.writeBits(codes.at(kEndSymbol), lengths->at(kEndSymbol));
    writer.finish();
    *payload = writer.data();
    *validBits = quint8(writer.pendingBits());
    return true;
}

bool encodeLiteralBlock(const QByteArray &raw, QVector<quint8> *lengths, QByteArray *payload,
                        quint8 *validBits)
{
    QVector<quint64> freq(kEndSymbol + 1, 0);
    for (char ch : raw)
        ++freq[quint8(ch)];
    ++freq[kEndSymbol];

    QVector<quint64> codes;
    if (!buildCodeLengths(freq, lengths) || !buildCanonical(*lengths, &codes))
        return false;

    BitWriter writer;
    for (char ch : raw)
        writer.writeBits(codes.at(quint8(ch)), lengths->at(quint8(ch)));
    writer.writeBits(codes.at(kEndSymbol), lengths->at(kEndSymbol));
    writer.finish();
    *payload = writer.data();
    *validBits = quint8(writer.pendingBits());
    return true;
}

bool encodeBlock(const QByteArray &raw, const std::atomic_bool *cancel, bool *canceled,
                 EncodedBlock *result, QString *error)
{
    result->crc = crc32(raw);
    QVector<Token> tokens = tokenize(raw, cancel, canceled);
    if (*canceled)
        return false;

    QVector<quint8> lzLengths;
    QByteArray lzPayload;
    quint8 lzValidBits = 0;
    QVector<quint8> literalLengths;
    QByteArray literalPayload;
    quint8 literalValidBits = 0;

    const bool lzOk = encodeTokens(tokens, ZPack::kCodeTableSize, &lzLengths, &lzPayload,
                                   &lzValidBits);
    const bool literalOk =
        encodeLiteralBlock(raw, &literalLengths, &literalPayload, &literalValidBits);
    if (!lzOk && !literalOk) {
        *error = QStringLiteral("构建 Huffman 码表失败。");
        return false;
    }

    const int lzCost = lzOk ? lzLengths.size() + lzPayload.size() : std::numeric_limits<int>::max();
    const int literalCost =
        literalOk ? literalLengths.size() + literalPayload.size()
                  : std::numeric_limits<int>::max();

    if (lzOk && lzCost <= literalCost && lzCost < raw.size()) {
        result->type = 2;
        result->lengths = lzLengths;
        result->data = lzPayload;
        result->validBits = lzValidBits;
        return true;
    }
    if (literalOk && literalCost < raw.size()) {
        result->type = 1;
        result->lengths = literalLengths;
        result->data = literalPayload;
        result->validBits = literalValidBits;
        return true;
    }

    result->type = 0;
    result->lengths.clear();
    result->data = raw;
    result->validBits = 0;
    return true;
}

void writeBlock(QByteArray *out, const EncodedBlock &block, quint32 rawSize)
{
    out->append(char(block.type));
    appendLE32(out, rawSize);
    appendLE32(out, quint32(block.data.size()));
    appendLE32(out, block.crc);
    appendLE16(out, quint32(block.lengths.size()));
    for (quint8 length : block.lengths)
        out->append(char(length));
    out->append(char(block.validBits));
    out->append(block.data);
}

// ---------------- 解压 ----------------

struct DecoderTables
{
    QVector<quint64> firstCode;
    QVector<int> lengthCounts;
    QVector<int> firstSymbol;
    QVector<quint16> sortedSymbols;
    int maxLength = 0;

    bool build(const QVector<quint8> &lengths)
    {
        return buildCanonical(lengths, &m_codes, &firstCode, &firstSymbol, &sortedSymbols,
                              &maxLength, &lengthCounts);
    }

    int decode(BitReader &reader) const
    {
        return decodeSymbol(reader, firstCode, lengthCounts, firstSymbol, sortedSymbols,
                            maxLength);
    }

private:
    QVector<quint64> m_codes;
};

bool decodeBlock(const QByteArray &data, const QVector<quint8> &lengths, quint8 type,
                 quint32 rawSize, const std::atomic_bool *cancel, QByteArray *output,
                 QString *error)
{
    if (type == 0) {
        if (quint32(data.size()) != rawSize) {
            *error = QStringLiteral("存储块大小与块头不一致。");
            return false;
        }
        *output = data;
        return true;
    }

    if (type != 1 && type != 2) {
        *error = QStringLiteral("未知的 ZPack 块类型：%1").arg(type);
        return false;
    }
    if (lengths.size() != 257 && lengths.size() != ZPack::kCodeTableSize) {
        *error = QStringLiteral("ZPack 码表长度无效：%1").arg(lengths.size());
        return false;
    }
    for (quint8 length : lengths) {
        if (length > kMaxCodeLength) {
            *error = QStringLiteral("ZPack 码字长度超出范围。");
            return false;
        }
    }

    DecoderTables tables;
    if (!tables.build(lengths) || tables.maxLength <= 0) {
        *error = QStringLiteral("ZPack 码表损坏，无法重建 Huffman 码。");
        return false;
    }

    output->clear();
    output->reserve(int(rawSize));
    BitReader reader(data, data.size() * 8);
    while (true) {
        if (cancel && cancel->load())
            return false;
        const int symbol = tables.decode(reader);
        if (symbol < 0) {
            *error = QStringLiteral("ZPack 位流提前结束或码字无效。");
            return false;
        }
        if (symbol == kEndSymbol)
            break;
        if (symbol < 256) {
            if (quint32(output->size()) >= rawSize) {
                *error = QStringLiteral("ZPack 解压输出超过块原始大小。");
                return false;
            }
            output->append(char(symbol));
            continue;
        }
        if (type != 2 || symbol < kLengthStart || symbol >= kDistanceStart) {
            *error = QStringLiteral("ZPack 长度符号无效。");
            return false;
        }

        const int lengthIndex = symbol - kLengthStart;
        if (lengthIndex < 0 || lengthIndex >= 29) {
            *error = QStringLiteral("ZPack 长度码超出范围。");
            return false;
        }
        quint32 extra = 0;
        if (kLengthExtra[lengthIndex] > 0
            && !reader.readBits(kLengthExtra[lengthIndex], &extra)) {
            *error = QStringLiteral("ZPack 长度附加位不足。");
            return false;
        }
        const int length = kLengthBase[lengthIndex] + int(extra);
        if (length < kMinMatch || length > kMaxMatch) {
            *error = QStringLiteral("ZPack 匹配长度无效：%1").arg(length);
            return false;
        }

        const int distanceSymbol = tables.decode(reader);
        if (distanceSymbol < kDistanceStart || distanceSymbol >= ZPack::kCodeTableSize) {
            *error = QStringLiteral("ZPack 距离符号无效。");
            return false;
        }
        const int distanceIndex = distanceSymbol - kDistanceStart;
        if (distanceIndex < 0 || distanceIndex >= 30) {
            *error = QStringLiteral("ZPack 距离码超出范围。");
            return false;
        }
        quint32 distanceExtra = 0;
        if (kDistanceExtra[distanceIndex] > 0
            && !reader.readBits(kDistanceExtra[distanceIndex], &distanceExtra)) {
            *error = QStringLiteral("ZPack 距离附加位不足。");
            return false;
        }
        const int distance = kDistanceBase[distanceIndex] + int(distanceExtra);
        if (distance <= 0 || distance > output->size()) {
            *error = QStringLiteral("ZPack 回溯距离超出已解压数据。");
            return false;
        }
        if (quint32(output->size() + length) > rawSize) {
            *error = QStringLiteral("ZPack 解压输出超过块原始大小。");
            return false;
        }

        const int source = output->size() - distance;
        for (int i = 0; i < length; ++i)
            output->append(output->at(source + i));
    }

    if (quint32(output->size()) != rawSize) {
        *error = QStringLiteral("ZPack 解压块大小不一致：期望 %1，实际 %2")
                     .arg(rawSize)
                     .arg(output->size());
        return false;
    }
    return true;
}

bool readHeader(QFile &file, quint64 *originalSize, quint32 *blockCount, QString *error)
{
    const QByteArray header = file.read(kHeaderSize);
    if (header.size() != kHeaderSize) {
        *error = QStringLiteral("不是有效的 ZPack 文件：文件头不完整。");
        return false;
    }
    if (header.left(4) != QByteArrayLiteral("STZ1")) {
        *error = QStringLiteral("不是 ZPack 文件：魔数不是 STZ1。");
        return false;
    }
    const int version = quint8(header.at(4));
    if (version != 1) {
        *error = QStringLiteral("不支持的 ZPack 版本：%1").arg(version);
        return false;
    }
    bool ok = true;
    *originalSize = readLE64(header, 5, &ok);
    *blockCount = readLE32(header, 13, &ok);
    if (!ok) {
        *error = QStringLiteral("ZPack 文件头损坏。");
        return false;
    }
    return true;
}

bool decodeArchive(const QString &archivePath, QFile *output, QString *error,
                   const std::atomic_bool *cancel, const ZPack::Progress &progress)
{
    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取 ZPack 文件：%1").arg(input.errorString());
        return false;
    }

    quint64 originalSize = 0;
    quint32 blockCount = 0;
    if (!readHeader(input, &originalSize, &blockCount, error))
        return false;

    quint64 written = 0;
    for (quint32 index = 0; index < blockCount; ++index) {
        if (cancel && cancel->load())
            return false;

        const QByteArray blockHeader = input.read(1 + 4 + 4 + 4 + 2);
        if (blockHeader.size() != 1 + 4 + 4 + 4 + 2) {
            *error = QStringLiteral("ZPack 块头不完整。");
            return false;
        }
        const quint8 type = quint8(blockHeader.at(0));
        bool ok = true;
        const quint32 rawSize = readLE32(blockHeader, 1, &ok);
        const quint32 dataSize = readLE32(blockHeader, 5, &ok);
        const quint32 expectedCrc = readLE32(blockHeader, 9, &ok);
        const quint16 tableSize = readLE16(blockHeader, 13, &ok);
        if (!ok || rawSize > 64u * 1024u * 1024u || dataSize > 64u * 1024u * 1024u) {
            *error = QStringLiteral("ZPack 块大小字段无效。");
            return false;
        }
        if (tableSize > ZPack::kCodeTableSize) {
            *error = QStringLiteral("ZPack 码表过大。");
            return false;
        }

        QVector<quint8> lengths;
        if (tableSize > 0) {
            const QByteArray table = input.read(tableSize);
            if (table.size() != tableSize) {
                *error = QStringLiteral("ZPack 码表不完整。");
                return false;
            }
            lengths.resize(tableSize);
            for (int i = 0; i < tableSize; ++i)
                lengths[i] = quint8(table.at(i));
        }

        const QByteArray validByte = input.read(1);
        if (validByte.size() != 1) {
            *error = QStringLiteral("ZPack 位数字段缺失。");
            return false;
        }
        const quint8 validBits = quint8(validByte.at(0));
        Q_UNUSED(validBits)

        const QByteArray data = input.read(dataSize);
        if (data.size() != int(dataSize)) {
            *error = QStringLiteral("ZPack 块数据不完整。");
            return false;
        }

        QByteArray decoded;
        if (!decodeBlock(data, lengths, type, rawSize, cancel, &decoded, error))
            return false;
        if (crc32(decoded) != expectedCrc) {
            *error = QStringLiteral("ZPack 块 %1 CRC32 校验失败。").arg(index + 1);
            return false;
        }
        if (output && output->write(decoded) != decoded.size()) {
            *error = QStringLiteral("写入解压文件失败：%1").arg(output->errorString());
            return false;
        }
        written += quint64(decoded.size());

        if (progress.report && blockCount > 0)
            progress.report(int((quint64(index + 1) * 100) / blockCount),
                            QStringLiteral("正在解压 ZPack 块 %1/%2")
                                .arg(index + 1)
                                .arg(blockCount));
    }

    if (written != originalSize) {
        *error = QStringLiteral("ZPack 解压后大小不一致：期望 %1，实际 %2")
                     .arg(originalSize)
                     .arg(written);
        return false;
    }
    if (input.pos() != input.size()) {
        *error = QStringLiteral("ZPack 文件末尾存在未识别数据。");
        return false;
    }
    return true;
}

double histogramDistance(const QVector<quint64> &left, quint64 leftTotal,
                         const QVector<quint64> &right, quint64 rightTotal)
{
    if (leftTotal == 0 || rightTotal == 0)
        return 0.0;
    double distance = 0.0;
    for (int i = 0; i < 256; ++i) {
        const double a = double(left.at(i)) / double(leftTotal);
        const double b = double(right.at(i)) / double(rightTotal);
        distance += std::fabs(a - b);
    }
    return distance;
}

QVector<quint64> histogram(const QByteArray &data)
{
    QVector<quint64> result(256, 0);
    for (char ch : data)
        ++result[quint8(ch)];
    return result;
}

} // namespace

namespace ZPack {

bool isZPackFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return file.read(4) == QByteArrayLiteral("STZ1");
}

QString suggestedOutputName(const QString &archivePath)
{
    const QFileInfo info(archivePath);
    QString name = info.completeBaseName();
    if (name.isEmpty())
        name = QStringLiteral("output");
    return name;
}

bool inspect(const QString &path, Info *info, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取 ZPack 文件：%1").arg(file.errorString());
        return false;
    }
    quint64 originalSize = 0;
    quint32 blockCount = 0;
    if (!readHeader(file, &originalSize, &blockCount, error))
        return false;

    Info result;
    result.originalSize = originalSize;
    result.blockCount = blockCount;
    result.compressedSize = quint64(file.size());

    for (quint32 index = 0; index < blockCount; ++index) {
        const QByteArray blockHeader = file.read(1 + 4 + 4 + 4 + 2);
        if (blockHeader.size() != 1 + 4 + 4 + 4 + 2) {
            *error = QStringLiteral("ZPack 块头不完整。");
            return false;
        }
        const quint8 type = quint8(blockHeader.at(0));
        bool ok = true;
        const quint32 dataSize = readLE32(blockHeader, 5, &ok);
        const quint16 tableSize = readLE16(blockHeader, 13, &ok);
        if (!ok) {
            *error = QStringLiteral("ZPack 块头损坏。");
            return false;
        }
        if (type == 0)
            ++result.storedBlocks;
        else if (type == 1)
            ++result.huffmanBlocks;
        else if (type == 2)
            ++result.lzBlocks;
        else {
            *error = QStringLiteral("ZPack 块类型无效：%1").arg(type);
            return false;
        }
        const qint64 skip = qint64(tableSize) + 1 + qint64(dataSize);
        if (file.pos() + skip > file.size() || !file.seek(file.pos() + skip)) {
            *error = QStringLiteral("ZPack 块数据不完整。");
            return false;
        }
    }
    if (info)
        *info = result;
    return true;
}

bool compressFile(const QString &sourcePath, const QString &outputPath, QString *error,
                  const std::atomic_bool *cancel, const Progress &progress)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        *error = QStringLiteral("ZPack 只能压缩单个普通文件：%1")
                     .arg(QDir::toNativeSeparators(sourcePath));
        return false;
    }
    if (QFileInfo(outputPath).absoluteFilePath()
        == sourceInfo.absoluteFilePath()) {
        *error = QStringLiteral("输出文件不能与源文件相同。");
        return false;
    }

    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取源文件：%1").arg(input.errorString());
        return false;
    }

    const QString tempPath = outputPath + QStringLiteral(".tmp");
    QFile::remove(tempPath);
    QFile output(tempPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("无法创建输出文件：%1").arg(output.errorString());
        return false;
    }

    const quint64 originalSize = quint64(sourceInfo.size());
    QByteArray header;
    header.append(QByteArrayLiteral("STZ1"));
    header.append(char(1));
    appendLE64(&header, originalSize);
    appendLE32(&header, 0);
    if (output.write(header) != header.size()) {
        *error = QStringLiteral("写入 ZPack 文件头失败。");
        output.close();
        QFile::remove(tempPath);
        return false;
    }

    quint32 blockCount = 0;
    qint64 written = 0;
    QByteArray block;
    QVector<quint64> previousHistogram(256, 0);
    quint64 previousTotal = 0;

    auto finishBlock = [&]() -> bool {
        if (block.isEmpty())
            return true;
        bool canceled = false;
        EncodedBlock encoded;
        if (!encodeBlock(block, cancel, &canceled, &encoded, error)) {
            if (canceled && cancel && cancel->load()) {
                *error = QStringLiteral("压缩操作已取消。");
            }
            return false;
        }
        QByteArray record;
        writeBlock(&record, encoded, quint32(block.size()));
        if (output.write(record) != record.size()) {
            *error = QStringLiteral("写入 ZPack 块失败：%1").arg(output.errorString());
            return false;
        }
        ++blockCount;
        written += record.size();
        block.clear();
        if (progress.report && originalSize > 0) {
            progress.report(int((quint64(input.pos()) * 100) / originalSize),
                            QStringLiteral("正在压缩 ZPack：%1%")
                                .arg(int((quint64(input.pos()) * 100) / originalSize)));
        }
        return true;
    };

    while (!input.atEnd()) {
        if (cancel && cancel->load()) {
            *error = QStringLiteral("压缩操作已取消。");
            output.close();
            QFile::remove(tempPath);
            return false;
        }
        const QByteArray window = input.read(kMinBlock);
        if (window.isEmpty())
            break;
        const QVector<quint64> currentHistogram = histogram(window);
        quint64 currentTotal = 0;
        for (quint64 value : currentHistogram)
            currentTotal += value;

        if (!block.isEmpty() && block.size() >= kMinBlock) {
            const bool forceCut = block.size() >= kMaxBlock;
            const double distance =
                histogramDistance(previousHistogram, previousTotal, currentHistogram,
                                  currentTotal);
            if (forceCut || distance > 0.42) {
                if (!finishBlock())
                    return false;
            }
        }

        block.append(window);
        previousHistogram = currentHistogram;
        previousTotal = currentTotal;
    }

    if (!finishBlock()) {
        output.close();
        QFile::remove(tempPath);
        return false;
    }

    if (!output.seek(13)) {
        *error = QStringLiteral("无法回写 ZPack 块数。");
        output.close();
        QFile::remove(tempPath);
        return false;
    }
    QByteArray countBytes;
    appendLE32(&countBytes, blockCount);
    if (output.write(countBytes) != countBytes.size()) {
        *error = QStringLiteral("写入 ZPack 块数失败。");
        output.close();
        QFile::remove(tempPath);
        return false;
    }
    output.close();
    if (output.error() != QFileDevice::NoError) {
        *error = QStringLiteral("关闭 ZPack 文件失败：%1").arg(output.errorString());
        QFile::remove(tempPath);
        return false;
    }
    if (QFileInfo::exists(outputPath))
        QFile::remove(outputPath);
    if (!QFile::rename(tempPath, outputPath)) {
        *error = QStringLiteral("保存 ZPack 文件失败：%1")
                     .arg(QDir::toNativeSeparators(outputPath));
        QFile::remove(tempPath);
        return false;
    }

    if (progress.report)
        progress.report(100, QStringLiteral("ZPack 压缩完成"));
    return true;
}

bool decompressFile(const QString &archivePath, const QString &outputPath, QString *error,
                    const std::atomic_bool *cancel, const Progress &progress)
{
    if (!isZPackFile(archivePath)) {
        *error = QStringLiteral("不是有效的 ZPack 文件。");
        return false;
    }
    const QString tempPath = outputPath + QStringLiteral(".tmp");
    QFile::remove(tempPath);
    QFile output(tempPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("无法创建解压文件：%1").arg(output.errorString());
        return false;
    }
    if (!decodeArchive(archivePath, &output, error, cancel, progress)) {
        output.close();
        QFile::remove(tempPath);
        return false;
    }
    output.close();
    if (output.error() != QFileDevice::NoError) {
        *error = QStringLiteral("关闭解压文件失败：%1").arg(output.errorString());
        QFile::remove(tempPath);
        return false;
    }
    if (QFileInfo::exists(outputPath))
        QFile::remove(outputPath);
    if (!QFile::rename(tempPath, outputPath)) {
        *error = QStringLiteral("保存解压文件失败：%1")
                     .arg(QDir::toNativeSeparators(outputPath));
        QFile::remove(tempPath);
        return false;
    }
    if (progress.report)
        progress.report(100, QStringLiteral("ZPack 解压完成"));
    return true;
}

bool testFile(const QString &archivePath, QString *error, const std::atomic_bool *cancel,
              const Progress &progress)
{
    return decodeArchive(archivePath, nullptr, error, cancel, progress);
}

} // namespace ZPack
