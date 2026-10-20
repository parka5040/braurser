braurser_add_test(NAME jobs-tests PREFIX Jobs SOURCES planner_test.cpp jobs_test.cpp srcinfo_test.cpp aur_test.cpp aur_hardening_test.cpp LIBS braurser-daemon-jobs)
target_compile_definitions(jobs-tests PRIVATE JOBS_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/helper-argv")
