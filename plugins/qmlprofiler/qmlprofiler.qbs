import qbs

QtcCommercialPlugin {
    name: "QmlProfilerExtension"

    Depends { name: "Core" }
    Depends { name: "QmlProfiler" }
    Depends { name: "Timeline" }

    Depends { name: "Qt.widgets" }

    files: [
        "debugmessagesmodel.cpp",
        "debugmessagesmodel.h",
        "inputeventsmodel.cpp",
        "inputeventsmodel.h",
        "memoryusagemodel.cpp",
        "memoryusagemodel.h",
        "pixmapcachemodel.cpp",
        "pixmapcachemodel.h",
        "qmlprofilerextensionconstants.h",
        "qmlprofilerextensionplugin.cpp",
        "qmlprofilerextensionplugin.h",
        "qmlprofilerextension_global.h",
        "scenegraphtimelinemodel.cpp",
        "scenegraphtimelinemodel.h",
    ]
}