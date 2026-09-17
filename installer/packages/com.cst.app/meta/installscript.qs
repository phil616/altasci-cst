function Component() {
    installer.setDefaultPageVisible(QInstaller.TargetDirectory, false);
}

Component.prototype.createOperations = function() {
    var required = installer.value("ApplicationsDirX64") + "/CST";
    if (installer.value("TargetDir").replace(/\\/g, "/").toLowerCase() !== required.toLowerCase())
        throw new Error("CST must be installed in " + required);
    component.createOperations();
    component.addOperation("CreateShortcut", "@TargetDir@/cst.exe", "@StartMenuDir@/Customer Service Terminal.lnk");
    component.addOperation("CreateShortcut", "@TargetDir@/cst.exe", "@DesktopDir@/Customer Service Terminal.lnk");
};
