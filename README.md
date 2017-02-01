# Introduction

Seti is a programming language that aims bring features from modern languages, 
as facility to manipulate data structures, object oriented programming, 
functional programming and others, to shell script.
```ruby
# rename to uppder case all txt files
%*.txt%.for_each(func(file) {
  mv ${file} ${file.to_upper()}
})
```

# Building

## Requirements:
  * A compiler that supports C++ 14 (gcc or clang)
  * Boost
  * Readline
  * CMake
  * Linux
  
## Compiling

### Fedora
```
# dnf install gcc-c++ clang
# dnf install boost boost-devel readline readline-devel cmake git
$ git clone https://github.com/alexst07/seti.git
$ cd seti
$ mkdir build && cd build
$ cmake ..
$ make
# make install
```

### Ubuntu
```
# apt-get install -y build-essential
# apt-get install -y libboost-all-dev libreadline6 libreadline6-dev git cmake
$ git clone https://github.com/alexst07/seti.git
$ cd seti
$ mkdir build && cd build
$ cmake ..
$ make
# make install
```

# Running
## Hello world
```
$ seti
> echo hello world
```
## Running a file
```
$ seti file.seti
```
