from libc.stddef cimport size_t
from libcpp.string cimport string

cdef extern from "<array>" namespace "std":
    cdef cppclass array_string_2 "std::array<std::string, 2>":
        string& operator[](size_t) except +

cdef extern from "<optional>" namespace "std":
    cdef cppclass optional[T]:
        bint has_value() const
        T& value() except +

cdef extern from "ray/util/network_util.h" namespace "ray":
    optional[array_string_2] ParseAddress(const string &address)
    string BuildAddress(const string &host, const string &port)
    string BuildAddress(const string &host, int port)
