braurser_add_test(NAME domain-tests PREFIX Domain SOURCES domain_test.cpp LIBS braurser-domain)
braurser_add_test(NAME vercmp-tests PREFIX Vercmp SOURCES vercmp_test.cpp LIBS braurser-domain)
braurser_add_test(NAME variant-tests PREFIX Variant SOURCES variant_test.cpp LIBS braurser-domain)
braurser_add_test(NAME restart-tests PREFIX Restart SOURCES restart_test.cpp LIBS braurser-domain)
