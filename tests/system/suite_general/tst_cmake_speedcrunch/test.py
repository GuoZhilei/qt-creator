source("../../shared/qtcreator.py")

SpeedCrunchPath = ""

def main():
    if(which("cmake") == None):
        test.fatal("cmake not found")
        return

    test.verify(os.path.exists(SpeedCrunchPath))

    startApplication("qtcreator" + SettingsPath)

    openCmakeProject(SpeedCrunchPath)

    waitFor("object.exists(':speedcrunch_QModelIndex')", 20000)

    # Test that some of the expected items are in the navigation tree
    for row, record in enumerate(testData.dataset("speedcrunch_tree.tsv")):
        node = testData.field(record, "node")
        value = testData.field(record, "value")
        test.compare(findObject(node).text, value)

    # Invoke a rebuild of the application
    invokeMenuItem("Build", "Rebuild All")

    # Wait for, and test if the build succeeded
    waitForBuildFinished(300000)

    invokeMenuItem("File", "Exit")

def init():
    global SpeedCrunchPath
    SpeedCrunchPath = SDKPath + "/creator-test-data/speedcrunch/src/CMakeLists.txt"
    cleanup()

def cleanup():
    # Make sure the .user files are gone
    if os.access(SpeedCrunchPath + ".user", os.F_OK):
        os.remove(SpeedCrunchPath + ".user")

    BuildPath = SDKPath + "/creator-test-data/speedcrunch/src/qtcreator-build"

    if os.access(BuildPath, os.F_OK):
        shutil.rmtree(BuildPath)
