check:
	valgrind --suppressions=pdfium.supp ./build/lexicon -v ~/pdf_container_test.vsf
