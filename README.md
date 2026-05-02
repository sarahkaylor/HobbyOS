# HobbyOS

A simple C language project created as a starting point for an OS.

## Setup

### Required Software Installation (macOS)

This project requires the following tools, which can be installed via Homebrew:

1. **Install Homebrew** (if not already installed):
   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. **Install LLVM** (for Clang compiler and LLD linker):
   ```bash
   brew install llvm lld
   ```

3. **Install QEMU** (for running the OS):
   ```bash
   brew install qemu
   ```

4. **Install dosfstools** (for creating FAT16 disk images):
   ```bash
   brew install dosfstools
   ```

5. **Install mtools** (for manipulating FAT disk images):
   ```bash
   brew install mtools
   ```

### Verify Installation

After installation, verify the tools are available:
```bash
/opt/homebrew/opt/llvm/bin/clang --version
/opt/homebrew/bin/qemu-system-aarch64 --version
```

## Build

Run `make` to compile the project:
```bash
make
```

This will:
- Compile all kernel source files from `src/kernel/`
- Compile user test programs from `src/user/`
- Create a FAT16 disk image with the test binaries
- Link the final `hobbyos.elf` kernel binary

### Build Individual Components

Build only the kernel:
```bash
make hobbyos.elf
```

Build user test programs:
```bash
make tests
```

### Clean Build Artifacts

Remove all compiled files and start fresh:
```bash
make clean
```

## Run

Run the OS in QEMU with the disk image:
```bash
make run
```

This launches QEMU with:
- ARM64 virt machine with Cortex-A53 CPU
- 128MB of RAM
- The kernel binary loaded directly
- Serial console output (ttyAMA0)
- VirtIO block device with the disk image

### QEMU Controls

When running in nographic mode:
- Press `Ctrl+A`, then `X` to exit QEMU

## Project Structure

```
HobbyOS/
├── src/
│   ├── kernel/          # Kernel source files
│   │   ├── main.c       # Entry point
│   │   ├── boot.s       # Boot assembly code
│   │   └── ...          # Other kernel components
│   └── include/         # Kernel headers
├── user/                # User-space test programs
│   ├── mem_test.c       # Memory allocation tests
│   ├── file_io_test.c   # File I/O tests
│   └── ...              # Other test programs
├── user_include/        # User-space headers
│   └── libc.h           # Minimal C library
├── linker.ld            # Kernel linker script
├── Makefile             # Build configuration
└── README.md            # This file
```

## License

https://opensource.org/license/mit

Copyright 2026 Sarah Kaylor

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

