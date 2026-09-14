#include "room/protocol/RoomProtocol.h"
#include "room/protocol/RevisionTracker.h"
#include "room/protocol/StateSubscription.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <iostream>
#include <stdexcept>
using namespace screenshare::room::wire;
QJsonArray Fixtures(const char* name) {
    QFile f(QString::fromUtf8(ROOM_V2_FIXTURES) + "/" + name + ".json");
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open fixtures");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) throw std::runtime_error("Invalid fixtures");
    return doc.array();
}
int main() {
    int failures = 0;
    auto check = [&](bool ok, const QString& name) {
        if (!ok) { ++failures; std::cerr << name.toStdString() << '\n'; }
    };
    for (const auto* file : {"commands", "events"}) for (auto entry : Fixtures(file)) {
        auto f = entry.toObject();
        auto m = f["message"].toObject();
        if (f.contains("repeat")) {
            auto r = f["repeat"].toObject(); auto p = m["payload"].toObject();
            p[r["field"].toString()] = r["text"].toString().repeated(r["count"].toInt()); m["payload"] = p;
        }
        QByteArray bytes;
        if (f.contains("hex")) bytes = QByteArray::fromHex(f["hex"].toString().toLatin1());
        else if (f.contains("raw")) bytes = f["raw"].toString().toUtf8();
        else if (f["message"].isArray()) bytes = QJsonDocument(f["message"].toArray()).toJson(QJsonDocument::Compact);
        else bytes = QJsonDocument(m).toJson(QJsonDocument::Compact);
        bytes += QByteArray(f["padding"].toInt(), ' ');
        auto result = QString(file) == "commands" ? ValidateClientCommand(bytes, f["scope"] == "directory") : ValidateServerEvent(bytes, f["scope"] == "directory");
        const auto name = f["name"].toString();
        check(result.ok == f["ok"].toBool(), name + " acceptance: " + result.error);
        if (f.contains("error")) check(result.error == f["error"].toString(), name + " error: " + result.error);
        if (f.contains("nickname")) check(result.message["payload"].toObject()["nickname"] == f["nickname"], name + " normalization");
    }
    RevisionTracker tracker;
    for (auto entry : Fixtures("revisions")) {
        auto f = entry.toArray(); auto event = f[0].toString();
        auto generation = static_cast<std::uint64_t>(f[1].toDouble());
        auto revision = static_cast<std::uint64_t>(f[2].toDouble());
        if (event == "start") check(tracker.Start() == generation, "generation");
        else if (event == "stop") tracker.Stop();
        else {
            auto d = event == "snapshot" ? tracker.Snapshot(generation, revision) : tracker.Delta(generation, revision);
            auto actual = d == RevisionTracker::Decision::Apply ? "apply" : d == RevisionTracker::Decision::Resync ? "resync" : "ignore";
            check(f[3].toString() == actual, event + " decision");
        }
        auto expected = f.size() > 4 ? f[4] : QJsonValue(QJsonValue::Null);
        check(expected.isNull() ? !tracker.Revision() : tracker.Revision() == static_cast<std::uint64_t>(expected.toDouble()), event + " revision");
    }
    RevisionTracker independent;
    independent.Start(); independent.Snapshot(1, 0);
    check(independent.Delta(1, 1) == RevisionTracker::Decision::Apply && tracker.Revision() == RevisionTracker::MaxRevision, "independent streams");
    for (auto trace : Fixtures("state-cache")) {
        auto t = trace.toObject(); StateSubscription cache(t["directory"].toBool());
        check(cache.Start("room", "viewer") == 1, "cache start");
        for (auto step : t["steps"].toArray()) {
            auto s = step.toObject();
            auto result = cache.Receive(static_cast<std::uint64_t>(s["generation"].toDouble()), QJsonDocument(s["message"].toObject()).toJson(QJsonDocument::Compact));
            const QStringList names{"ignore", "applied", "resync", "invalid", "closed"};
            check(names[static_cast<int>(result)] == s["result"], "cache decision " + s["result"].toString());
            check(cache.Payload() == s["payload"].toObject(), "cache atomic payload");
            check(s["revision"].isNull() ? !cache.Revision() : cache.Revision() == static_cast<std::uint64_t>(s["revision"].toDouble()), "cache revision");
        }
        cache.Stop();
        check(cache.Payload().isEmpty() && !cache.Revision(), "cache stop clears state");
        check(cache.Receive(1, "{") == StateSubscription::Result::Ignore, "stale callback ignored before parsing");
        check(cache.Start("room", "viewer") > 1, "cache reconnect generation");
    }
    std::cout << Fixtures("commands").size() << " command fixtures, " << Fixtures("events").size()
        << " event fixtures and revision/state-cache traces; " << failures << " failures\n";
    return failures ? 1 : 0;
}
