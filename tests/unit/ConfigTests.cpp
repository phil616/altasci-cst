#include "application/ProjectConfigService.h"
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace cst;
class ConfigTests final : public QObject {
    Q_OBJECT
private slots:
    void persistence() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ProjectConfigService config(ConfigurationValidator(readJson(CST_SOURCE_DIR "/config/cst-project.schema.json"),
                                  {"C:/Program Files/CST", "C:/ProgramData/CST", "C:/Windows"}));
        ProjectCatalogService catalog(directory.path(), config);
        QVERIFY(catalog.entries().isEmpty());
        const auto example = config.load(CST_SOURCE_DIR "/config/cst-project.example.json");
        const auto id = catalog.importProject(example);
        QCOMPARE(catalog.entries().size(), 1);
        QCOMPARE(catalog.defaultId(), id);
        QCOMPARE(catalog.selectedId(), id);
        QCOMPARE(catalog.project(id), example);
        QVERIFY_EXCEPTION_THROWN(catalog.importProject(example), std::runtime_error);
        catalog.exportProject(id, directory.filePath("export.json"));
        QCOMPARE(config.load(directory.filePath("export.json")), example);
        auto changed = example; auto project = changed["project"].toObject(); project["name"] = "新名称"; changed["project"] = project;
        catalog.saveProject(id, changed);
        QCOMPARE(catalog.entries()[0].name, "新名称");
        auto invalid = changed; invalid["unknown"] = "bad";
        QVERIFY_EXCEPTION_THROWN(catalog.saveProject(id, invalid), ConfigurationError);
        QCOMPARE(catalog.project(id), changed);
        catalog.removeProject(id);
        QVERIFY(catalog.entries().isEmpty());
        QVERIFY(catalog.defaultId().isEmpty());
        QVERIFY_EXCEPTION_THROWN(catalog.project(id), std::runtime_error);
        QVERIFY(QFile::exists(directory.filePath("export.json")));
    }
    void invalidCatalog() {
        QTemporaryDir directory;
        ProjectConfigService config(ConfigurationValidator(readJson(CST_SOURCE_DIR "/config/cst-project.schema.json"), {}));
        ProjectCatalogService catalog(directory.path(), config);
        saveJson(directory.filePath("catalog.json"), {{"projects", QJsonArray{}}, {"defaultProjectId", "missing"}, {"lastSelectedId", ""}});
        QVERIFY_EXCEPTION_THROWN(catalog.entries(), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(catalog.project("../../file"), std::runtime_error);
    }
    void draftRequiresRealInputs() {
        ProjectConfigService config(ConfigurationValidator(readJson(CST_SOURCE_DIR "/config/cst-project.schema.json"), {}));
        const auto draft = config.create("新项目", "C:/projects/new", "C:/Git/git.exe");
        QVERIFY(!config.validate(draft).isEmpty());
        QVERIFY(!draft["project"].toObject()["id"].toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ConfigTests)
#include "ConfigTests.moc"
