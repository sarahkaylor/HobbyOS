%define _binary_payload w9.gzdio
Name:           mtools
Summary:        mtools, read/write/list/format DOS disks under Unix
Version:        4.0.49
Release:        1
License:        GPLv3+
Group:          Utilities/System
URL:            http://www.gnu.org/software/mtools/
Source:         http://ftp.gnu.org/gnu/%{name}/%{name}-%{version}.tar.gz
Buildroot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)


%description
Mtools is a collection of utilities to access MS-DOS disks from GNU
and Unix without mounting them. It supports long file names, OS/2 Xdf
disks, ZIP/JAZ disks and 2m disks (store up to 1992k on a high density
3 1/2 disk).


%prep
%setup -q

./configure \
    --prefix=%{buildroot}%{_prefix} \
    --sysconfdir=/etc \
    --infodir=%{buildroot}%{_infodir} \
    --mandir=%{buildroot}%{_mandir} \
    --enable-floppyd \

%build
make

%clean
echo rm -rf $RPM_BUILD_ROOT
[ X%{buildroot} != X ] && [ X%{buildroot} != X/ ] && rm -fr %{buildroot}

%install
make install
make install-info
strip %{buildroot}%{_bindir}/mtools %{buildroot}%{_bindir}/mkmanifest %{buildroot}%{_bindir}/floppyd
rm %{buildroot}%{_infodir}/dir

%files
%defattr(-,root,root)
%{_infodir}/mtools.info*
%{_mandir}/man1/floppyd.1*
%{_mandir}/man1/floppyd_installtest.1.gz
%{_mandir}/man1/mattrib.1*
%{_mandir}/man1/mbadblocks.1*
%{_mandir}/man1/mcat.1*
%{_mandir}/man1/mcd.1*
%{_mandir}/man1/mcopy.1*
%{_mandir}/man1/mdel.1*
%{_mandir}/man1/mdeltree.1*
%{_mandir}/man1/mdoctorfat.1*
%{_mandir}/man1/mdir.1*
%{_mandir}/man1/mdu.1*
%{_mandir}/man1/mformat.1*
%{_mandir}/man1/minfo.1*
%{_mandir}/man1/mkmanifest.1*
%{_mandir}/man1/mlabel.1*
%{_mandir}/man1/mmd.1*
%{_mandir}/man1/mmount.1*
%{_mandir}/man1/mmove.1*
%{_mandir}/man1/mpartition.1*
%{_mandir}/man1/mrd.1*
%{_mandir}/man1/mren.1*
%{_mandir}/man1/mshortname.1*
%{_mandir}/man1/mshowfat.1*
%{_mandir}/man1/mtools.1*
%{_mandir}/man5/mtools.5*
%{_mandir}/man1/mtoolstest.1*
%{_mandir}/man1/mtype.1*
%{_mandir}/man1/mzip.1*
%{_bindir}/amuFormat.sh
%{_bindir}/mattrib
%{_bindir}/mbadblocks
%{_bindir}/mcat
%{_bindir}/mcd
%{_bindir}/mcopy
%{_bindir}/mdel
%{_bindir}/mdeltree
%{_bindir}/mdir
%{_bindir}/mdoctorfat
%{_bindir}/mdu
%{_bindir}/mformat
%{_bindir}/minfo
%{_bindir}/mkmanifest
%{_bindir}/mlabel
%{_bindir}/mmd
%{_bindir}/mmount
%{_bindir}/mmove
%{_bindir}/mpartition
%{_bindir}/mrd
%{_bindir}/mren
%{_bindir}/mshortname
%{_bindir}/mshowfat
%{_bindir}/mtools
%{_bindir}/mtoolstest
%{_bindir}/mtype
%{_bindir}/mzip
%{_bindir}/floppyd
%{_bindir}/floppyd_installtest
%{_bindir}/mcheck
%{_bindir}/mcomp
%{_bindir}/mxtar
%{_bindir}/tgz
%{_bindir}/uz
%{_bindir}/lz
%doc NEWS

%pre
groupadd floppy 2>/dev/null || echo -n ""

%post
if [ -f %{_bindir}/install-info ] ; then
	if [ -f %{_infodir}/dir ] ; then
		%{_bindir}/install-info %{_infodir}/mtools.info %{_infodir}/dir
	fi
	if [ -f %{_infodir}/dir.info ] ; then
		%{_bindir}/install-info %{_infodir}/mtools.info %{_infodir}/dir.info
	fi
fi


%preun
install-info --delete %{_infodir}/mtools.info %{_infodir}/dir.info
if [ -f %{_bindir}/install-info ] ; then
	if [ -f %{_infodir}/dir ] ; then
		%{_bindir}/install-info --delete %{_infodir}/mtools.info %{_infodir}/dir
	fi
	if [ -f %{_infodir}/dir.info ] ; then
		%{_bindir}/install-info --delete %{_infodir}/mtools.info %{_infodir}/dir.info
	fi
fi

%changelog
* Sat Jun 14 2025 Alain Knaff <alain@knaff.lu>
- Correctly detect overlong file names, especially when
  multibyte (UTF-8) strings are concerned
- Fix off-by-one error in 2 calls to native_to_wchar
- When changing disk label, ignore any plain file which would
  match label
- Added missing newlines in error messages about disk geometry
- Do not print error message if getting geometry is simply not
  supported by device type (such as for loop devices)
- If full geometry of a device is not available (such as for
  loop devices), at least make use of the size
* Sat Feb 22 2025 Alain Knaff <alain@knaff.lu>
- portability fixes, mostly for Watcom compiler
- cleaned up some other warnings
- various runtime fixes for windows (/dev/tty vs CONIN$)
- use correct mtools (sub)command name in usage() when using -c
- other usage() spelling/wording fixes
- fix mtype'ing multiple files at once
- integrate mkmanpages into make system, clean it, and credit
  texi2roff's original developer and others
- document compilation on less usual environments in INSTALL
- uninitialized buffer fix in mformat
- fix parameter counting error in mcd, and document that image
  file name is not remembered
* Sun Jan 19 2025 Alain Knaff <alain@knaff.lu>
- fixed some of the manpage issues reported by manpage-l10n
- unix-dir copying on Windows fix
- new no-daemon mode for floppyd to make testing easier
- make floppyd independant of XauFileName (portability)
- autoconf fixes
* Thu Nov 21 2024 Alain Knaff <alain@knaff.lu>
- iconv buffer overflow fixes
- removed references to mread and mwrite (obsolete subcommands
  from mcopy)
- documented mdoctorfat, and addressed 2 bugs/oversights
- removed references to obsolete mread and mwrite
- portability fixes (dietlibc and MacOS X) & simplification
* Sat Sep 28 2024 Alain Knaff <alain@knaff.lu>
- Fixed iconv descriptor leak
- Fixed size of error message buffer
* Sun Jun 02 2024 Alain Knaff <alain@knaff.lu>
- Added documentation for size parameters
- Fix parsing of fat_start (reserved sectors) in mformat.c so
  as to allow more than 255
- Rewrite autorename in vfat.c such that it doesn't
  (temporarily) overwrite byte after name string
- Switch statement fall-through fixes (size parsing, and bios disk in
  mformat.c)
- Compilation warning fixes, mostly for CLANG
* Tue Mar 21 2023 Alain Knaff <alain@knaff.lu>
- Fix root directory test in mattrib
- -b BiosDisk flag for mformat to allow setting physdrive to a user-specified
  value
- Clearer error message in mformat when trying to mformat a disk whose total
  size is not known
- Make recursive copy more consistent
- Trailing slash now always implies target should be a directory
- Code cleanup
* Sat Oct 22 2022 Alain Knaff <alain@knaff.lu>
- Added postcmd attribute in drive description to allow to
  execute "device release" code automatically at end of command
- Code cleanup (unneeded functions, initializations, added
  comments to unobvious code, obsolete stuff in Makefile)
- signedness cleanup about directory entries
* Sun Sep 18 2022 Alain Knaff <alain@knaff.lu>
- Made it possible again to have FAT32 filesystems with less
  than 0xfff5 clusters
- Make FAT32 entries 0 and 1 match what windows 10 does
- Misc source code and configure script cleanup
* Sat Jun 04 2022 Alain Knaff <alain@knaff.lu>
- Remove libbsd dependency
- Better compatibility with legacy platforms such as AT&T UnixPC
- Upgraded to autoconf 2.71
* Sun Apr 10 2022 Alain Knaff <alain@knaff.lu>
- Rename strtoi to strosi (string to signed int). The strtoi function
  on BSD does something else (returns an intmax, not an int)
* Thu Mar 03 2022 Alain Knaff <alain@knaff.lu>
- Make sure case byte is cleared when making the special
  directory entries "." and ".."
* Sun Dec 26 2021 Alain Knaff <alain@knaff.lu>
- Removed mclasserase commands, which doesn't fit the coding
  structure of the rest of mtools
- Add support to -i option to mcd
- Document -i in mtools.1
- Fix a missing command error in floppyd_io.c
* Sun Nov 21 2021 Alain Knaff <alain@knaff.lu>
- Fix error status of recursive listing of empty root directory
- If recursive listing, also show matched files at level one
- Use "seekless" reads & write internally, where possible
- Text mode conversion refactoring
- Misc refactoring
* Fri Aug 06 2021 Alain Knaff <alain@knaff.lu>
- Fix cluster padding at end of file in batch mode, and add comments about what
  happens here
* Fri Jul 23 2021 Alain Knaff <alain@knaff.lu>
- Fix mcopy -s issue
* Sat Jul 17 2021 Alain Knaff <alain@knaff.lu>
- Fix support for partitions (broken in 4.0.30)
- Portability fixes for Solaris 10 and 11
- General simplification of configure script, and largefile handling
- Tested and fixed for platforms *without* largefile support
- In cases where lseek works with 32-bit offsets, prefer lseek64 over llseek
- Fixed floppy sector size handling on platforms that are not Linux
- Added support for image files on command line to mcat
* Sat Jul 10 2021 Alain Knaff <alain@knaff.lu>
- Simplify algorithm that choses filesystem parameters for
  format, and align it more closely with what Win7 does
- Fix mformatting XDF when XDF not explicitly specified on
  mformat command line
- easier way to enter sizes on mformat command line (mformat -C -T 1440K)
- For small sizes, mformat assumes floppy geometries (heads 1 or 2,
  tracks 40 or 80)
- Handle attempts to mformat too small filesystems more gracefully
- Enable minfo to print out additional mformat command line
  parameters, if the present filesystem uses non-default values for
  these
- minfo no longer prints bigsect if smallsect is set
- for remap filter, error when trying to write non-zero data to
unmapped sectors
- Fix misc compilation warnings occuring when disabling certain
features (largefiles, raw-term)

* Sat Jun 19 2021 Alain Knaff <alain@knaff.lu>
- Move Linux-specific block device sizing code into
  linux-specific section of devices.c
- Error messages for all failure cases on fs_init failure
- Fix compilation without XDF support (OpenImage signature)
- Fix polarity of format_xdf command-line parameter of mformat
- In XDF_IO retry enough times to actually succeed, even if
  FDC was in a bad state before
- Remove useless buffer flushing triggered when giving up a
  reference to a stream node that is still referenced
  elsewhere.
- Clearer error message if neither size nor geometry of drive
  to be mformatted is known
- In mformat, make Fs dynamically allocated rather than
  on-stack, so as to be able to use utilities supplied by
  stream.c
- Remove duplicate writing of backup boot sector
- Allow to infer geometry if only size is specified
- Protect against attempt to create zero-sized buffer
- Code simplification in mattrib
- Remove dead code in mpartition

* Thu Jun 17 2021 Alain Knaff <alain@knaff.lu>
- Fixed XDF floppy disk access
- Fixed faulty behavior at end of image in mcat
- Device/Image size handling refactoring
- allow remap to write to zero-backed sectors (may happen if
  buffer is flushed, and is not an error in that case)
- Raise an error when trying to mcopy multiple source files
  over a single destination file (rather than directory)
- fix handling of "hidden" sectors (is a 2 byte quantity on
  small disks, not 4 byte as previously assumed)
- Modernize partition support. Tuned consistency check to
  actually check about important issues (such as overlapping
  partitions) rather than stuff nobody else cares about
  (alignment on entire cylinder boundaries)
- Move various "filter" options (partition, offset, swap,
  scsi) into separate classes, rather than leaving almost
  everything in plain_io
- Simplify and centralize geometry handling and LBA code
- Fix some more more compiler warnings
* Mon May 31 2021 Alain Knaff <alain@knaff.lu>
-Fix bug in cluster preallocation, which was accidentally introduced by compiler warning "fixes" from v4_0_28
* Sat Nov 28 2020 Alain Knaff <alain@knaff.lu>
- Fix compilation on Macintosh
- Ignore image file locking errors if we are performing a read-only access anyways
- Minor man-page fixes
* Sat Oct 24 2020 Alain Knaff <alain@knaff.lu>
- Preserve non-updated contents of info sector, just in case it contains program code
- When parsing config file, always use "C" locale for case-insensitive comparisons
* Sun Mar 22 2020 Alain Knaff <alain@knaff.lu>
- Spelling fixes in documentation
- Permit calling "make install" with >= -j2
- Added AC_SYS_LARGEFILE, needed for compiling on certain ARM procs
* Sun Dec 09 2018 Alain Knaff <alain@knaff.lu>
- Address lots of compiler warnings (assignments between different types)
- Network speedup fixes for floppyd (TCP_CORK)
- Typo fixes
- Explicitly pass available target buffer size for character set conversions
* Sun Dec 02 2018 Alain Knaff <alain@knaff.lu>
- Fixed -f flag for mformat (size is KBytes, rather than sectors)
- Fixed toupper/tolower usage (unsigned char rather than plain signed)
* Sat Nov 24 2018 Alain Knaff <alain@knaff.lu>
- Fixed compilation for MingW
- After MingW compilation, make sure executable has .exe extension
- Addressed compiler warnings
- Fixed length handling in character set conversion (Unicode file names)
- Fixed matching of character range, when containing Unicode characters (mdir "c:test[α-ω].exe")
- Fixed initialization of my_scsi_cmd constructor
* Sun Nov 11 2018 Alain Knaff <alain@knaff.lu>
- initialize directory entries to 0
- bad message "Too few sectors" replaced with "Too many sectors"
- apostrophe in mlabel no longer causes generation of long entry
- option to fake system date for file creation using the SOURCE_DATE_EPOCH environment variables
- can now be compiled with "clang" compiler
- fallback function for strndup, for those platforms that do not have it
- fixed a number of -Wextra warnings
- new compressed archive formats for uz/lz
- allow to specify number of reserved sectors for FAT32.
- file/device locking with timeout (rather than immediate failure)
- fixed support for BPB-less legacy formats.
- removed check that disk must be an integer number of tracks.
- removed .eh/.oh macros from manual pages
* Sat Sep 29 2018 Alain Knaff <alain@knaff.lu>
- Fix for short file names starting with character 0xE5	(by remapping it to 0x5)
- mpartition: Partition types closer to what Microsoft uses
- mformat: figure out LBA geometry as last resort if geometry
is neither specified in config and/or commandline, nor can be
queried from the device
- mformat: use same default cluster size by size as Microsoft for FAT32
- additional sanity checks
- document how cluster size is picked in mformat.c man page
- document how partition types are picked in mpartition.c man page
* Wed Jan 09 2013 Alain Knaff <alain@knaff.lu>
- Fix for names of iconv encodings on AIX
- Fix mt_size_t on NetBSD
- Fixed compilation on Mingw
- Fixed doc (especially mformat)
- Fix mformating of FAT12 filesystems with huge cluster sizes
- Minfo prints image file name in mformat command line if an image
- file name was given
- Always generate gzip-compressed RPMs, in order to remain
- compatible with older distributions
- Fixed buffer overflow with drive letter in mclasserase
* Wed Jun 29 2011 Alain Knaff <alain@knaff.lu>
- mbadblocks now takes a list of bad blocks (either as sectors
  or as clusters)
- mbadblocks now is able to do write scanning for bad blocks
- mshowfat can show cluster of specific offset
- Enable mtools to deal with very small sector sizes...
- Fixed encoding of all-lowercase names (no need to mangle
  these)
- Consider every directory entry after an ENDMARK (0x00) to be deleted
- After writing a new entry at end of a directory, be sure to also add
  an ENDMARK (0x00)
- Deal with possibility of a NULL pointer being returned by
  localtime during timestamp conversion
* Sat Apr 16 2011 Alain Knaff <alain@knaff.lu>
- configure.in fixes
- fixed formatting of fat_size_calculation.tex document
- compatibility with current autoconfig versions
- Make it clear that label is limited to 11 characters
- Fixed typo in initialization of FAT32 info sector
* Sun Oct 17 2010 Alain Knaff <alain@knaff.lu>
- Added missing -i option to mshortname
* Sun Oct 17 2010 Alain Knaff <alain@knaff.lu>
- Released v4_0_14:
- New mshortname command
- Fix floppyd for disks bigger than 2 Gig
- Remove obsolete -z flag
- Remove now unsupported AC_USE_SYSTEM_EXTENSIONS
- Fixed output formatting of mdir if MTOOLS_DOTTED_DIR is set
- Mformat now correctly writes backup boot sector
- Fixed signedness of serial number in mlabel
- Fixed buffer size problem in mlabel
- Make mlabel write backup boot sector if FAT32
- Catch situation where both clear and new label are given to mlabel
- Quote filename parameters to scripts
- Mformat: Close file descriptor for boot sector
- Added lzip support to scripts/uz
- Added Tot_sectors option to mformat
- Fixed hidden sector handling in mformat
- Minfo generates mformat command lines containing new -T option
- Mlabel prints error if label too long
* Sun Feb 28 2010 Alain Knaff <alain@knaff.lu>
- Merged Debian patches
* Tue Nov 03 2009 Alain Knaff <alain@knaff.lu>
- Mingw compatibility fixes
