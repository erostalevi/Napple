/* sfdnsres core: resolver error strings (portable). Text follows the FreeBSD netdb strings the Horizon resolver returns. */
#pragma once
const char *sfdns_hstrerror(unsigned netdb_error);  /* GetHostStringErrorRequest */
const char *sfdns_gai_strerror(unsigned gai_error); /* GetGaiStringErrorRequest */
