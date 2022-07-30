#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//double_free
void double_free(void)
{
	char *p = malloc(100);
	free(p);
	free(p);
}

//stack_corrupt
void __stack_corrupt(char **p)
{
	char __p[1];
	*p = __p;
}

void stack_corrupt(void)
{
	char *p;
	__stack_corrupt(&p);
	printf("%p\n", p);
	memset(p, 'a', 10);
	printf("%c\n", *p);
}

void overwrite(void)
{
	char *p = malloc(100);
	memset(p, 0xff, 101);
	free(p);
}


int main(void)
{
	//double_free();
	//stack_corrupt();
	overwrite();
	return 0;
}
