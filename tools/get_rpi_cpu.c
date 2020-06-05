#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char bits;

struct rev_s {
	union {
		struct {
			/* NOQuuuWuFMMMCCCCPPPPTTTTTTTTRRRR */
			bits revision : 4;
			bits type : 8;
			bits processor : 4;
			bits manufacturer : 4;
			bits memory_size : 3;
			bits new_flag : 1;
			bits unused_1 : 1;
			bits warranty : 1;
			bits unused_2 : 3;
			bits otp_read : 1;
			bits otp_program : 1;
			bits overvoltage : 1;
		} __attribute__ ((__packed__)) part;

		long int revhex;
	};
};

void print_rpi_cpu(const char *revlin)
{
	const char *revstr = strrchr(revlin, ' ');
	struct rev_s rev;
	const char *proc[] = { "BCM2835", "BCM2836", "BCM2837", "BCM2711" };

	if(revstr == NULL)
		return;

	rev.revhex = strtol(revstr, NULL, 16);
	if(rev.part.new_flag == 0)
		return;

	printf("%s\n", proc[rev.part.processor]);
}

int main(int argc, char *argv[])
{
	char buf[255];
	FILE *f;

	if(argc != 2)
		return EXIT_FAILURE;

	f = fopen(argv[1], "r");
	if(f == NULL)
		return EXIT_FAILURE;

	while(fgets(buf, sizeof(buf), f) != NULL)
	{
		const char revtxt[] = "Revision";
		if(strncmp(buf, revtxt, strlen(revtxt)) != 0)
			continue;

		print_rpi_cpu(buf);
		break;
	}

	fclose(f);
	return EXIT_SUCCESS;
}
