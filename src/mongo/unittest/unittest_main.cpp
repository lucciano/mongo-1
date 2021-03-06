// mongo/unittest/unittest_main.cpp

#include <stdio.h>
#include <string>
#include <vector>

#include "mongo/util/log.h"
#include "base/initializer.h"
#include "mongo/unittest/unittest.h"

int main( int argc, char **argv, char **envp ) {
    mongo::Logstream::setLogFile(stderr);

    ::mongo::runGlobalInitializersOrDie(argc, argv, envp);
    return ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);
}

void mongo::unittest::onCurrentTestNameChange( const std::string &testName ) {}
