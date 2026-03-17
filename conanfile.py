from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class CppThreadPoolConan(ConanFile):
    name = "cpp-thread-pool"
    version = "0.1.0"
    package_type = "library"

    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "include/*", "src/*", "cmake/*"

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["thread_pool"]

