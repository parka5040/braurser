braurser_add_fuzzer(NAME fuzz_pkgname SOURCES fuzz_pkgname.cpp LIBS braurser-validate)
braurser_add_fuzzer(NAME fuzz_frame SOURCES fuzz_frame.cpp LIBS braurser-protocol)
braurser_add_fuzzer(NAME fuzz_syncdb SOURCES fuzz_syncdb.cpp LIBS braurser-daemon-catalog)
braurser_add_fuzzer(NAME fuzz_wiki_html SOURCES fuzz_wiki_html.cpp LIBS braurser-daemon-wiki)
