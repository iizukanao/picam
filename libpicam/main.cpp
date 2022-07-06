#include <iostream>

#include "libpicam/picam.hpp"

int main(int argc, char *argv[])
{
    Picam* picam;
    return picam->getInstance().run(argc, argv);
}
