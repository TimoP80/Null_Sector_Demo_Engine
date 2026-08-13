// ---------------------------------------------------------------------------
// NULL SECTOR // DEMO EDITOR - standalone editor shell.
//
// The actual runtime setup and DemoEditor wiring live in src/main.cpp. This
// tiny entry point only supplies the editor-specific executable identity and
// forwards the normal engine CLI to the shared application entry point.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <string>
#include <vector>

extern int nsDemoMain(int argc, char** argv);

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
      std::printf("NULL SECTOR // DEMO EDITOR\n"
                  "  Opens the standalone dockable production editor.\n"
                  "  All normal engine options are accepted.\n\n"
                  "  ns_editor.exe --demo=data/demo.nsd\n"
                  "  ns_editor.exe --demo=data/my_show.nsd --window=1680x960\n"
                  "  ns_editor.exe --demo=data/my_show.nsd --no-track\n"
                  "  ns_editor.exe --editor-seconds=5   (CI smoke mode)\n");
      return 0;
    }
  }

  // Keep argument storage alive while the shared entry point parses it. The
  // forced --editor switch is appended so standalone-editor behavior wins even
  // if a caller accidentally omits it; all other options pass through intact.
  std::vector<std::string> storage;
  storage.reserve((size_t)argc + 1);
  for (int i = 0; i < argc; ++i) storage.emplace_back(argv[i]);
  bool hasEditorFlag = false;
  for (size_t i = 1; i < storage.size(); ++i)
    if (storage[i] == "--editor") hasEditorFlag = true;
  if (!hasEditorFlag) storage.emplace_back("--editor");

  std::vector<char*> forwarded;
  forwarded.reserve(storage.size());
  for (std::string& arg : storage)
    forwarded.push_back(arg.data());
  return nsDemoMain((int)forwarded.size(), forwarded.data());
}
