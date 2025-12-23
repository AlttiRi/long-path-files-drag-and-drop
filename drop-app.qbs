CppApplication {
    name: "DragDropLongPathApp"
    targetName: "DragDropApp"
    consoleApplication: false

    cpp.cxxLanguageVersion: "c++20"
    cpp.minimumWindowsVersion: "10.0"

    files: [
        "*.cpp",
        "app.manifest",
        "app_res.rc",
    ]

    cpp.dynamicLibraries: [
        "shell32",  // SHGetPathFromIDListEx, DragQueryFileW
        "ole32",    // OleInitialize
        "uuid",     // IID_IUnknown, IID_IDropTarget
        "user32",   // WndProc, CreateWindowExW, DefWindowProcW (MSVC)
    ]

    cpp.defines: [
        "UNICODE",
        "_UNICODE",
        "WIN32_LEAN_AND_MEAN",
    ]

    cpp.driverFlags: {
        if (qbs.toolchain.contains("mingw")) {
            return [ "-mwindows", "-municode" ];
        }
        return [];
    }
}
