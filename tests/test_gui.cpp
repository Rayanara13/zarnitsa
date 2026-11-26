// Окно в offscreen: добавление из строки, выделение щелчком, пауза пробелом, темы, экран дежурного, карточка.
#include "core/Monitor.h"
#include "service/Engine.h"
#include "ui/DetailDialog.h"
#include "ui/HeaderBar.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/TileView.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QtTest>

using namespace zr;

class TestGui : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void addAndInteract();
    void demoRendersAllModes();
    void headerFitsNarrowWindow();

private:
    QTemporaryDir m_dir;
};

void TestGui::initTestCase()
{
    QVERIFY(m_dir.isValid());
    Theme::init();
    Theme::apply(qApp, QStringLiteral("dark"));
}

void TestGui::addAndInteract()
{
    Engine engine(m_dir.filePath(QStringLiteral("gui.json")), false);
    QString err;
    QVERIFY2(engine.load(&err), qPrintable(err));
    MainWindow w(&engine);
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    w.addFromText(QStringLiteral("10.0.0.1 # Шлюз\n10.0.0.2\nsip:10.0.0.3\n"));
    QCOMPARE(engine.monitor()->targets().size(), 3);
    w.addFromText(QStringLiteral("не адрес"), false);
    QCOMPARE(engine.monitor()->targets().size(), 3);
    w.addFromText(QStringLiteral("10.0.0.4"));
    QCOMPARE(engine.monitor()->targets().size(), 4);

    TileView *tiles = w.tiles();
    tiles->clearSelection();
    QApplication::processEvents();
    // первая плитка начинается под шапкой группы: отступ 16 + шапка 36 + 6
    QTest::mouseClick(tiles->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(60, 90));
    QCOMPARE(tiles->selection().size(), 1);
    const QString first = tiles->selection().first();
    QCOMPARE(engine.monitor()->target(first)->name, QStringLiteral("Шлюз"));

    QTest::keyClick(tiles, Qt::Key_Space);
    QCOMPARE(engine.monitor()->runtime(first)->status, Status::Paused);
    QTest::keyClick(tiles, Qt::Key_Space);
    QCOMPARE(engine.monitor()->runtime(first)->status, Status::Unknown);

    QTest::keySequence(tiles, QKeySequence::SelectAll);
    QCOMPARE(tiles->selection().size(), 4);

    tiles->setDensity(TileView::Density::List);
    tiles->setFilterText(QStringLiteral("10.0.0.3"));
    QCOMPARE(tiles->visibleIds().size(), 1);
    tiles->setFilterText(QString());
    QCOMPARE(tiles->visibleIds().size(), 4);

    w.openDetails(first);
    QApplication::processEvents();
    DetailDialog *d = w.findChild<DetailDialog *>();
    QVERIFY(d);
    QVERIFY(d->isVisible());
    d->close();

    engine.save();
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("gui.json"))));
}

void TestGui::demoRendersAllModes()
{
    Engine engine(QString(), true);
    QString err;
    QVERIFY(engine.load(&err));
    MainWindow w(&engine);
    w.resize(1440, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(engine.monitor()->summary().down > 0);
    for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        Theme::apply(qApp, theme);
        for (TileView::Density den : {TileView::Density::Comfortable, TileView::Density::Compact, TileView::Density::List}) {
            w.tiles()->setDensity(den);
            const QImage img = w.grab().toImage();
            QVERIFY(!img.isNull());
            QCOMPARE(img.width(), w.width() * int(img.devicePixelRatio()));
            // в окне есть и красный (лежит), и зелёный (в норме)
            bool red = false, green = false;
            for (int y = 0; y < img.height(); y += 3)
                for (int x = 0; x < img.width(); x += 3) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 200 && c.green() < 90 && c.blue() < 90) red = true;
                    if (c.green() > 170 && c.red() < 80 && c.blue() < 130) green = true;
                }
            QVERIFY2(red && green, qPrintable(theme));
        }
    }
    w.setWall(true);
    QVERIFY(w.isWall());
    w.setWall(false);
    QVERIFY(!w.isWall());
}

void TestGui::headerFitsNarrowWindow()
{
    Engine engine(QString(), true);
    QString err;
    QVERIFY(engine.load(&err));
    HeaderBar header(&engine);
    header.setFixedHeight(header.sizeHint().height());
    header.show();
    for (int width : {760, 1024, 1440}) {
        header.resize(width, header.height());
        QApplication::processEvents();
        QVERIFY(!header.grab().isNull());
        const auto children = header.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *a : children) {
            if (!a->isVisible()) continue;
            QVERIFY2(header.rect().contains(a->geometry()), qPrintable(QStringLiteral("Ширина %1: элемент вышел за шапку").arg(width)));
            for (QWidget *b : children) {
                if (a == b || !b->isVisible()) continue;
                QVERIFY2(!a->geometry().intersects(b->geometry()), qPrintable(QStringLiteral("Ширина %1: элементы шапки перекрываются").arg(width)));
            }
        }
        QVERIFY(header.search()->isVisible());
    }
}

QTEST_MAIN(TestGui)
#include "test_gui.moc"
