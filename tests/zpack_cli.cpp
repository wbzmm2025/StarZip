// ZPack 命令行调试/基准工具。产品功能仍通过 StarZip GUI 与 ArchiveWorker 使用。
#include "ZPack.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

QString sha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

int usage()
{
    QTextStream(stderr) << "usage: ZPackTool <compress|decompress|test|info|roundtrip> <input> [output]\n";
    return 2;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 3)
        return usage();

    const QString mode = args.at(1);
    const QString input = args.at(2);
    QString error;
    ZPack::Progress progress;
    progress.report = [](int percent, const QString &message) {
        if (percent == 0 || percent == 100 || percent % 10 == 0)
            QTextStream(stdout) << "[" << percent << "%] " << message << "\n";
    };

    if (mode == QStringLiteral("compress")) {
        if (args.size() < 4)
            return usage();
        const QString output = args.at(3);
        if (!ZPack::compressFile(input, output, &error, nullptr, progress)) {
            QTextStream(stderr) << "FAIL: " << error << "\n";
            return 1;
        }
        const qint64 inSize = QFileInfo(input).size();
        const qint64 outSize = QFileInfo(output).size();
        QTextStream(stdout) << "OK ratio="
                            << (inSize > 0 ? double(outSize) * 100.0 / double(inSize) : 0.0)
                            << "% in=" << inSize << " out=" << outSize << "\n";
        return 0;
    }

    if (mode == QStringLiteral("decompress")) {
        if (args.size() < 4)
            return usage();
        const QString output = args.at(3);
        if (!ZPack::decompressFile(input, output, &error, nullptr, progress)) {
            QTextStream(stderr) << "FAIL: " << error << "\n";
            return 1;
        }
        QTextStream(stdout) << "OK sha256=" << sha256(output) << "\n";
        return 0;
    }

    if (mode == QStringLiteral("test")) {
        if (!ZPack::testFile(input, &error, nullptr, progress)) {
            QTextStream(stderr) << "FAIL: " << error << "\n";
            return 1;
        }
        QTextStream(stdout) << "OK\n";
        return 0;
    }

    if (mode == QStringLiteral("info")) {
        ZPack::Info info;
        if (!ZPack::inspect(input, &info, &error)) {
            QTextStream(stderr) << "FAIL: " << error << "\n";
            return 1;
        }
        QTextStream(stdout) << "original=" << info.originalSize
                            << " compressed=" << info.compressedSize
                            << " blocks=" << info.blockCount
                            << " stored=" << info.storedBlocks
                            << " huffman=" << info.huffmanBlocks
                            << " lz=" << info.lzBlocks << "\n";
        return 0;
    }

    if (mode == QStringLiteral("roundtrip")) {
        QTemporaryDir dir;
        if (!dir.isValid()) {
            QTextStream(stderr) << "FAIL: cannot create temp dir\n";
            return 1;
        }
        const QString packed = dir.filePath(QStringLiteral("data.stz"));
        const QString restored = dir.filePath(QStringLiteral("restored.bin"));
        if (!ZPack::compressFile(input, packed, &error, nullptr, progress)) {
            QTextStream(stderr) << "compress FAIL: " << error << "\n";
            return 1;
        }
        if (!ZPack::decompressFile(packed, restored, &error, nullptr, progress)) {
            QTextStream(stderr) << "decompress FAIL: " << error << "\n";
            return 1;
        }
        const QString a = sha256(input);
        const QString b = sha256(restored);
        if (a.isEmpty() || a != b) {
            QTextStream(stderr) << "FAIL: hash mismatch\n";
            return 1;
        }
        const qint64 inSize = QFileInfo(input).size();
        const qint64 outSize = QFileInfo(packed).size();
        QTextStream(stdout) << "OK ratio="
                            << (inSize > 0 ? double(outSize) * 100.0 / double(inSize) : 0.0)
                            << "%\n";
        return 0;
    }

    return usage();
}
