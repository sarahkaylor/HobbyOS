// HobbyOS sysroot compat: fstream explicit instantiations for the LLVM
// toolchain cross-build (wave-2 lane W2-LLVM).
//
// The OS-staged libc++ (third_party/libcxx-21.1.8, __config_site
// _LIBCPP_HAS_FILESYSTEM=0) deliberately compiles out the ENTIRE <fstream>
// implementation, but clang's Frontend unconditionally needs std::ifstream
// (CompilerInvocation.cpp:4678, LayoutOverrideSource.cpp:43,
// CrossTranslationUnit.cpp).  In libc++21 <fstream> itself is header-only
// (templates + inline members); only the three <char> explicit
// instantiations (extern template in fstream:1598-1601, guarded by
// _LIBCPP_AVAILABILITY_HAS_ADDITIONAL_IOSTREAM_EXPLICIT_INSTANTIATIONS_1)
// must come from a library.  This TU supplies exactly those, compiled
// against the staged __config_site WITH filesystem enabled (see
// docs/selfhost/llvm-bootstrap.md §6 wave-2 notes), so the toolchain's own
// link closure gets the symbols without any LLVM source patch and without
// rebuilding the OS-side libcxx.a.  L2 work item: decide whether fstream
// belongs on in the OS libc++ config.
#include <fstream>

template class std::basic_ifstream<char>;
template class std::basic_ofstream<char>;
template class std::basic_filebuf<char>;
