/*
 * main.c -- Entry point. Parses argv, builds config, starts server.
 */

#include "cserve.h"

int
main(int argc, char **argv)
{
    server_config_t cfg;
    cs_config_defaults(&cfg);
    cs_config_parse_args(&cfg, argc, argv);
    cs_server_run(&cfg);
    return 0;
}
