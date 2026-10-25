
set(testdir ${CMAKE_CURRENT_SOURCE_DIR})

# GDB remote protocol test with minimal Python RSP client
add_test(NAME gdbstub-rsp
         COMMAND ${Python3_EXECUTABLE} ${testdir}/gdbstub_test.py
                 $<TARGET_FILE:${APP_NAME}> ${testdir}/gdbstub_test.tos)

# NOTE: vasm stores the absolute build directory as DWARF DW_AT_comp_dir
# in gdbstub_test.elf.  Before committing a rebuilt ELF, replace it with
# "." padded with NULs to the same length (keeps DWARF offsets valid), e.g.:
#   python3 -c "import os; p='gdbstub_test.elf'; d=open(p,'rb').read();
#     o=os.getcwd().encode()+b'\0';
#     open(p,'wb').write(d.replace(o, b'.'+b'\0'*(len(o)-1)))"

# Same with real (m68k capable) GDB, skipped if GDB isn't found
add_test(NAME gdbstub-gdb
         COMMAND ${Python3_EXECUTABLE} ${testdir}/gdbstub_gdb_test.py
                 $<TARGET_FILE:${APP_NAME}> ${testdir}/gdbstub_test.tos
                 ${testdir}/gdbstub_test.elf)
set_tests_properties(gdbstub-gdb PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
set_tests_properties(gdbstub-rsp PROPERTIES TIMEOUT 120)
