/*
 * Channel-tree lifecycle regression harness.
 *
 * This harness deliberately does NOT force the SDK-impossible ordering
 * "user-joined before channel-new". Per TeamTalk.h the SDK posts all
 * CLIENTEVENT_CMD_CHANNEL_NEW events before user events at login, and
 * TT_DoJoinChannel emits CHANNEL_NEW followed by USER_JOINED.
 *
 * Instead it reproduces the sequence the real Windows crashes come from:
 * the client drains the whole SDK message queue in one pass
 * (MainWindow::timerEvent -> while(TT_GetMessage(...)) processTTMessage(...)),
 * and a disconnect handled in the middle of that drain calls
 * ChannelsTree::resetChannels(), which deletes the entire tree. Every
 * remaining already-queued user event is then delivered against a tree
 * that no longer has any channel items.
 *
 * Enabled only with TEAMTALK_CHANNELTREE_TEST=1. Headless: the harness runs
 * before MainWindow::show() and is intended to be run with
 * QT_QPA_PLATFORM=offscreen.
 */

#include "channelstree.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QTreeWidgetItem>
#include <QVector>

#include <string.h>

#define CTR_ID_MASK      0xFFFF
#define CTR_CHANNEL_TYPE 0x10000
#define CTR_USER_TYPE    0x20000

namespace {

int g_pass = 0, g_fail = 0, g_warnings = 0;
QStringList g_log;
QString g_logpath;

struct Row { int userid; int chanid; QString name; };

void flushLog()
{
    if (g_logpath.isEmpty())
        return;
    QFile f(g_logpath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    QTextStream ts(&f);
    for (const QString& l : g_log)
        ts << l << "\n";
    ts.flush();
    f.close();
}

void msgHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
    {
        g_warnings++;
        if (g_log.size() < 4000)
            g_log << (QString("WARN  ") + msg);
        if (type == QtFatalMsg)
        {
            g_log << (QString("FATAL abort (Q_ASSERT or qFatal): ") + msg);
            flushLog();
        }
    }
}

void check(bool ok, const QString& name, const QString& detail = QString())
{
    if (ok) { g_pass++; g_log << ("PASS  " + name); }
    else    { g_fail++; g_log << ("FAIL  " + name + (detail.isEmpty() ? QString() : ("  [" + detail + "]"))); }
    // Flush eagerly: a baseline run crashes mid-scenario, and the last line
    // written is what identifies the step that crashed.
    flushLog();
}

void step(const QString& s) { g_log << ("STEP  " + s); flushLog(); }

void note(const QString& s) { g_log << ("....  " + s); }
void section(const QString& s) { g_log << ""; g_log << ("== " + s + " =="); }

void setStr(TTCHAR* dst, const QString& s)
{
    memset(dst, 0, TT_STRLEN * sizeof(TTCHAR));
#if defined(UNICODE) || defined(_UNICODE)
    s.left(TT_STRLEN - 1).toWCharArray(dst);
#else
    QByteArray b = s.left(TT_STRLEN - 1).toUtf8();
    memcpy(dst, b.constData(), size_t(b.size()));
#endif
}

Channel mkChannel(int id, int parent, const QString& name)
{
    Channel c = {};
    c.nChannelID = id;
    c.nParentID  = parent;
    setStr(c.szName, name);
    c.nMaxUsers  = 1000;
    return c;
}

User mkUser(int id, int chanid, const QString& nick)
{
    User u = {};
    u.nUserID    = id;
    u.nChannelID = chanid;
    setStr(u.szNickname, nick);
    setStr(u.szUsername, nick.toLower());
    return u;
}

void collect(QTreeWidgetItem* item, QVector<Row>& rows)
{
    if (!item)
        return;
    int chanid = (item->type() & CTR_CHANNEL_TYPE) ?
        (item->data(0, Qt::UserRole).toInt() & CTR_ID_MASK) : -1;
    for (int i = 0; i < item->childCount(); ++i)
    {
        QTreeWidgetItem* c = item->child(i);
        if (c->type() & CTR_USER_TYPE)
        {
            Row r;
            r.userid = c->data(0, Qt::UserRole).toInt() & CTR_ID_MASK;
            r.chanid = chanid;
            QString disp = c->data(0, Qt::DisplayRole).toString();
            int comma = disp.indexOf(QChar(0x2C));
            r.name = (comma >= 0 ? disp.left(comma) : disp).trimmed();
            rows.push_back(r);
        }
        else
            collect(c, rows);
    }
}

QVector<Row> rowsOf(ChannelsTree* t)
{
    QVector<Row> rows;
    for (int i = 0; i < t->topLevelItemCount(); ++i)
        collect(t->topLevelItem(i), rows);
    return rows;
}

int countUser(const QVector<Row>& rows, int userid)
{
    int n = 0;
    for (const Row& r : rows) if (r.userid == userid) n++;
    return n;
}

int totalItems(QTreeWidgetItem* item)
{
    if (!item) return 0;
    int n = 1;
    for (int i = 0; i < item->childCount(); ++i)
        n += totalItems(item->child(i));
    return n;
}

int treeItems(ChannelsTree* t)
{
    int n = 0;
    for (int i = 0; i < t->topLevelItemCount(); ++i)
        n += totalItems(t->topLevelItem(i));
    return n;
}

/* Populate exactly the way the SDK orders a login:
 * all CHANNEL_NEW first, then user login + user joined. */
void populate(ChannelsTree* tree, int nchannels, int nusers,
              QVector<Channel>& chans, QVector<User>& users)
{
    // A real login always follows a reset (first connect, or
    // disconnectFromServer() before reconnecting), so start clean.
    tree->resetChannels();
    chans.clear(); users.clear();
    chans.push_back(mkChannel(1, 0, "root"));
    for (int c = 2; c <= nchannels; ++c)
        chans.push_back(mkChannel(c, 1, QString("chan%1").arg(c, 3, 10, QChar('0'))));
    for (const Channel& c : chans)
        tree->slotAddChannel(c);

    for (int i = 0; i < nusers; ++i)
    {
        int uid = 1000 + i;
        int cid = chans[(i % chans.size())].nChannelID;
        User u = mkUser(uid, cid, QString("user%1").arg(i, 4, 10, QChar('0')));
        users.push_back(u);
        tree->slotUserLoggedIn(u);
        tree->slotUserJoin(cid, u);
    }
}

} // namespace

int channelTreeRegressionRun(ChannelsTree* tree)
{
    qInstallMessageHandler(msgHandler);
    g_logpath = qEnvironmentVariable("TEAMTALK_CHANNELTREE_TEST_LOG");
    int cycles = qEnvironmentVariableIntValue("TEAMTALK_CHANNELTREE_TEST_CYCLES");
    if (cycles <= 0) cycles = 25;
    int nusers = qEnvironmentVariableIntValue("TEAMTALK_CHANNELTREE_TEST_USERS");
    if (nusers <= 0) nusers = 60;

    QElapsedTimer timer; timer.start();
    g_log << "=== channel tree lifecycle regression harness ===";
    g_log << QString("cycles=%1 users=%2").arg(cycles).arg(nusers);

    QVector<Channel> chans; QVector<User> users;

    section("S1 normal login population (SDK order: channels then users)");
    populate(tree, 6, nusers, chans, users);
    {
        QVector<Row> rows = rowsOf(tree);
        check(rows.size() == nusers, "S1 all users present",
              QString("got %1 want %2").arg(rows.size()).arg(nusers));
        bool dupes = false;
        for (const User& u : users) if (countUser(rows, u.nUserID) != 1) dupes = true;
        check(!dupes, "S1 no duplicate or missing user rows");
    }

    section("S2 user ordering is alphabetical and channels precede users");
    {
        bool ordered = true, grouping = true;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            QVector<QTreeWidgetItem*> stack;
            stack.push_back(tree->topLevelItem(i));
            while (!stack.isEmpty())
            {
                QTreeWidgetItem* it = stack.takeLast();
                QString prev; bool seenChannel = false;
                for (int c = 0; c < it->childCount(); ++c)
                {
                    QTreeWidgetItem* ch = it->child(c);
                    if (ch->type() & CTR_CHANNEL_TYPE)
                    {
                        seenChannel = true;
                        stack.push_back(ch);
                    }
                    else if (ch->type() & CTR_USER_TYPE)
                    {
                        // getUserIndex() stops at the first CHANNEL_TYPE child,
                        // so users are inserted ahead of any sub-channel. A user
                        // appearing after a sub-channel is an ordering defect.
                        if (seenChannel) grouping = false;
                        QString disp = ch->data(0, Qt::DisplayRole).toString();
                        int comma = disp.indexOf(QChar(0x2C));
                        QString name = (comma >= 0 ? disp.left(comma) : disp).trimmed();
                        if (!prev.isEmpty() && name.compare(prev, Qt::CaseInsensitive) < 0)
                            ordered = false;
                        prev = name;
                    }
                }
            }
        }
        check(ordered,  "S2 users sorted case-insensitively within channel");
        check(grouping, "S2 users are listed before sub-channels");
    }

    /* THE REAL CRASH SEQUENCE. */
    section("S3 disconnect resets tree, then already-queued events drain");
    {
        User joiner  = users[nusers / 2];
        User updater = users[0];
        User leaver  = users[1];

        step("S3 about to resetChannels()");
        tree->resetChannels();
        check(treeItems(tree) == 0, "S3 reset emptied the tree");

        step("S3 about to slotUserJoin on empty tree (baseline crashes here)");
        tree->slotUserJoin(joiner.nChannelID, joiner);
        check(true, "S3 queued slotUserJoin after reset did not crash");

        step("S3 about to slotUserUpdate on empty tree");
        tree->slotUserUpdate(updater);
        check(true, "S3 queued slotUserUpdate after reset did not crash");

        step("S3 about to slotUserLeft on empty tree");
        tree->slotUserLeft(leaver.nChannelID, leaver);
        check(true, "S3 queued slotUserLeft after reset did not crash");

        step("S3 about to slotUserLoggedOut on empty tree");
        tree->slotUserLoggedOut(leaver);
        check(true, "S3 queued slotUserLoggedOut after reset did not crash");

        check(treeItems(tree) == 0, "S3 tree still empty after stale backlog",
              QString("items=%1").arg(treeItems(tree)));
        // The user cache must not be repopulated by a stale backlog: a release
        // build silently default-inserts through m_users[id] without the guard.
        check(tree->getUsers().size() == 0, "S3 user cache not repopulated by stale backlog",
              QString("cached=%1").arg(tree->getUsers().size()));
    }

    section("S4 reconnect after stale backlog must not resurrect ghosts");
    {
        QVector<Channel> c2; QVector<User> u2;
        populate(tree, 6, nusers, c2, u2);
        QVector<Row> rows = rowsOf(tree);
        check(rows.size() == nusers, "S4 exactly the reconnected users are present",
              QString("got %1 want %2").arg(rows.size()).arg(nusers));
        bool dupes = false;
        for (const User& u : u2) if (countUser(rows, u.nUserID) != 1) dupes = true;
        check(!dupes, "S4 no duplicated or ghost rows after reconnect");
    }

    section("S5 channel removed, then queued user events for that channel");
    {
        Channel gone = chans[3];
        QVector<int> before = tree->getUsersInChannel(gone.nChannelID);
        tree->slotRemoveChannel(gone);
        User u = mkUser(4242, gone.nChannelID, "ghostuser");
        tree->slotUserLoggedIn(u);
        tree->slotUserJoin(gone.nChannelID, u);
        check(true, "S5 queued join for a removed channel did not crash");
        QVector<Row> rows = rowsOf(tree);
        check(countUser(rows, 4242) == 0, "S5 user for removed channel is not inserted");
        note(QString("channel had %1 users before removal").arg(before.size()));
    }

    section("S6 duplicate and stale events");
    {
        User u = users[2];
        tree->slotUserJoin(u.nChannelID, u);
        check(true, "S6 duplicate join did not crash");
        QVector<Row> rows = rowsOf(tree);
        // Upstream slotUserJoin() does not look for an existing item, so a
        // repeated USER_JOINED inserts a second row for the same user. That is
        // a separate pre-existing defect, outside the scope of the crash fix.
        // Recorded here rather than asserted so the suite reports the crash fix
        // honestly. TeamTalkPlus already guards this with a getUserItem() check.
        note(QString("KNOWN UPSTREAM DEFECT: duplicate USER_JOINED produced %1 row(s) for user %2")
                 .arg(countUser(rows, u.nUserID)).arg(u.nUserID));

        tree->slotUserLeft(u.nChannelID, u);
        tree->slotUserLeft(u.nChannelID, u);
        check(true, "S6 double leave did not crash");

        User unknown = mkUser(31337, 99, "nosuchchannel");
        tree->slotUserJoin(99, unknown);
        check(true, "S6 join for unknown channel did not crash");
        rows = rowsOf(tree);
        check(countUser(rows, 31337) == 0, "S6 unknown-channel user not inserted");
    }

    section("S7 mass population, disconnect mid-drain, reconnect");
    {
        QVector<Channel> c3; QVector<User> u3;
        populate(tree, 12, nusers * 4, c3, u3);
        int populated = rowsOf(tree).size();
        check(populated == nusers * 4, "S7 mass population complete",
              QString("got %1").arg(populated));

        tree->resetChannels();
        for (int i = 0; i < u3.size(); i += 7)
            tree->slotUserJoin(u3[i].nChannelID, u3[i]);
        for (int i = 1; i < u3.size(); i += 11)
            tree->slotUserUpdate(u3[i]);
        for (int i = 2; i < u3.size(); i += 13)
            tree->slotUserLeft(u3[i].nChannelID, u3[i]);
        check(treeItems(tree) == 0, "S7 large stale backlog left the tree empty",
              QString("items=%1").arg(treeItems(tree)));

        QVector<Channel> c4; QVector<User> u4;
        populate(tree, 12, nusers, c4, u4);
        check(rowsOf(tree).size() == nusers, "S7 reconnect population is exact",
              QString("got %1 want %2").arg(rowsOf(tree).size()).arg(nusers));
    }

    section("S8 repeated reset/rebuild cycles (growth and leak check)");
    {
        QVector<Channel> c5; QVector<User> u5;
        populate(tree, 6, nusers, c5, u5);
        int baseline = treeItems(tree);
        for (int i = 0; i < cycles; ++i)
        {
            tree->resetChannels();
            tree->slotUserJoin(u5[i % u5.size()].nChannelID, u5[i % u5.size()]);
            tree->slotUserUpdate(u5[(i + 1) % u5.size()]);
            QVector<Channel> cc; QVector<User> uu;
            populate(tree, 6, nusers, cc, uu);
        }
        int after = treeItems(tree);
        check(after == baseline, "S8 item count stable across reset/rebuild cycles",
              QString("baseline=%1 after=%2 cycles=%3").arg(baseline).arg(after).arg(cycles));
    }

    section("S9 bounded reentrancy: nested reset during queued delivery");
    {
        QVector<Channel> c6; QVector<User> u6;
        populate(tree, 4, 20, c6, u6);
        User pending = u6[5];
        tree->resetChannels();
        tree->slotUserJoin(pending.nChannelID, pending);
        tree->resetChannels();
        tree->slotUserUpdate(pending);
        check(true, "S9 nested reset + queued events did not crash");
        check(treeItems(tree) == 0, "S9 tree empty after nested resets");
    }

    section("S10 final rebuild sanity");
    {
        QVector<Channel> c7; QVector<User> u7;
        populate(tree, 6, nusers, c7, u7);
        QVector<Row> rows = rowsOf(tree);
        check(rows.size() == nusers, "S10 final population exact",
              QString("got %1 want %2").arg(rows.size()).arg(nusers));
        bool dupes = false;
        for (const User& u : u7) if (countUser(rows, u.nUserID) != 1) dupes = true;
        check(!dupes, "S10 no duplicates in final tree");
        tree->resetChannels();
        check(treeItems(tree) == 0, "S10 final reset clean");
    }

    g_log << "";
    g_log << QString("elapsed ms         : %1").arg(timer.elapsed());
    g_log << QString("qt warnings        : %1").arg(g_warnings);
    g_log << QString("checks passed      : %1").arg(g_pass);
    g_log << QString("checks failed      : %1").arg(g_fail);
    g_log << QString("RESULT             : %1").arg(g_fail == 0 ? "ALL PASS" : "FAILURES");
    flushLog();
    return g_fail == 0 ? 0 : 1;
}
