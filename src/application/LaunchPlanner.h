#pragma once
#include "Platform.h"
#include "domain/Configuration.h"

namespace cst {
// Configuration decoding ends here. The backend receives only a materialized plan.
class LaunchPlanner {
public:
    LaunchPlanner(IProcessRunner &runner, ProjectPaths paths);
    QJsonObject expand(const QJsonObject &value, const QJsonObject &project) const;
    ProcessSpec resolve(const QJsonObject &project, const QJsonObject &task,
                        const QJsonObject &command, const QString &runId) const;
private:
    Environment environment(const QJsonObject &project, const QJsonObject &task,
                            const QJsonObject &command) const;
    IProcessRunner &runner_;
    ProjectPaths paths_;
};
}
