#ifndef BONNY_AUTOSTART_H
#define BONNY_AUTOSTART_H

namespace JisuQi {

// Create (enable) or delete (disable) the Startup-folder shortcut for the
// currently running executable. Returns false only on hard failures.
bool SetAutoStart(bool enable);

} // namespace JisuQi

#endif // BONNY_AUTOSTART_H
