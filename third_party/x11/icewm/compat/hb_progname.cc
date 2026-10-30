// getprogname() for the HobbyOS IceWM build: IceWM's own ApplicationName
// (set from argv[0] by check_argv(), misc.cc) is the program name.
extern char const* ApplicationName;
extern "C" const char* getprogname(void) {
    return ApplicationName ? ApplicationName : "icewm";
}
