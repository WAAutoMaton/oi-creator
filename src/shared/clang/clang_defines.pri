DEFINES += CLANG_VERSION=\\\"$${LLVM_VERSION}\\\"
CLANG_RESOURCE_DIR=$$clean_path($${LLVM_LIBDIR}/clang/$${LLVM_VERSION}/include)
DEFINES += "\"CLANG_RESOURCE_DIR=\\\"$${CLANG_RESOURCE_DIR}\\\"\""
