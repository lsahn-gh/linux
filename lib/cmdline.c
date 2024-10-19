// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/lib/cmdline.c
 * Helper functions generally used for parsing kernel command line
 * and module options.
 *
 * Code and copyrights come from init/main.c and arch/i386/kernel/setup.c.
 *
 * GNU Indent formatting options for this file: -kr -i8 -npsl -pcs
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>

/*
 *	If a hyphen was found in get_option, this will handle the
 *	range of numbers, M-N.  This will expand the range and insert
 *	the values[M, M+1, ..., N] into the ints array in get_options.
 */

static int get_range(char **str, int *pint, int n)
{
	int x, inc_counter, upper_range;

	(*str)++;
	upper_range = simple_strtol((*str), NULL, 0);
	inc_counter = upper_range - *pint;
	for (x = *pint; n && x < upper_range; x++, n--)
		*pint++ = x;
	return inc_counter;
}

/**
 *	get_option - Parse integer from an option string
 *	@str: option string
 *	@pint: (optional output) integer value parsed from @str
 *
 *	Read an int from an option string; if available accept a subsequent
 *	comma as well.
 *
 *	When @pint is NULL the function can be used as a validator of
 *	the current option in the string.
 *
 *	Return values:
 *	0 - no int in string
 *	1 - int found, no subsequent comma
 *	2 - int found including a subsequent comma
 *	3 - hyphen found to denote a range
 *
 *	Leading hyphen without integer is no integer case, but we consume it
 *	for the sake of simplification.
 */

int get_option(char **str, int *pint)
{
	char *cur = *str;
	int value;

	if (!cur || !(*cur))
		return 0;
	if (*cur == '-')
		value = -simple_strtoull(++cur, str, 0);
	else
		value = simple_strtoull(cur, str, 0);
	if (pint)
		*pint = value;
	if (cur == *str)
		return 0;
	if (**str == ',') {
		(*str)++;
		return 2;
	}
	if (**str == '-')
		return 3;

	return 1;
}
EXPORT_SYMBOL(get_option);

/*
 * IAMROOT, 2021.10.16:
 * - cmd string의 경우 param=value 인것이 존재하고 그냥 param만 인것이 있다.
 *   또한 argument들이 space들로 분리되어있다. (param1 parm2 ...)
 *   param="abc def" 이렇게 value가 있는 경우도 있어 "를 해석해 value를 가져오는것도
 *   보인다.
 *   -- option들도 존재한다.
 */
/**
 *	get_options - Parse a string into a list of integers
 *	@str: String to be parsed
 *	@nints: size of integer array
 *	@ints: integer array (must have room for at least one element)
 *
 *	This function parses a string containing a comma-separated
 *	list of integers, a hyphen-separated range of _positive_ integers,
 *	or a combination of both.  The parse halts when the array is
 *	full, or when no more numbers can be retrieved from the
 *	string.
 *
 *	When @nints is 0, the function just validates the given @str and
 *	returns the amount of parseable integers as described below.
 *
 *	Returns:
 *
 *	The first element is filled by the number of collected integers
 *	in the range. The rest is what was parsed from the @str.
 *
 *	Return value is the character in the string which caused
 *	the parse to end (typically a null terminator, if @str is
 *	completely parseable).
 */

char *get_options(const char *str, int nints, int *ints)
{
	bool validate = (nints == 0);
	int res, i = 1;

	while (i < nints || validate) {
		int *pint = validate ? ints : ints + i;

		res = get_option((char **)&str, pint);
		if (res == 0)
			break;
		if (res == 3) {
			int n = validate ? 0 : nints - i;
			int range_nums;

			range_nums = get_range((char **)&str, pint, n);
			if (range_nums < 0)
				break;
			/*
			 * Decrement the result by one to leave out the
			 * last number in the range.  The next iteration
			 * will handle the upper number in the range
			 */
			i += (range_nums - 1);
		}
		i++;
		if (res == 1)
			break;
	}
	ints[0] = i - 1;
	return (char *)str;
}
EXPORT_SYMBOL(get_options);

/**
 *	memparse - parse a string with mem suffixes into a number
 *	@ptr: Where parse begins
 *	@retptr: (output) Optional pointer to next char after parse completes
 *
 *	Parses a string into a number.  The number stored at @ptr is
 *	potentially suffixed with K, M, G, T, P, E.
 */

unsigned long long memparse(const char *ptr, char **retptr)
{
	char *endptr;	/* local pointer to end of parsed string */

	unsigned long long ret = simple_strtoull(ptr, &endptr, 0);

	switch (*endptr) {
	case 'E':
	case 'e':
		ret <<= 10;
		fallthrough;
	case 'P':
	case 'p':
		ret <<= 10;
		fallthrough;
	case 'T':
	case 't':
		ret <<= 10;
		fallthrough;
	case 'G':
	case 'g':
		ret <<= 10;
		fallthrough;
	case 'M':
	case 'm':
		ret <<= 10;
		fallthrough;
	case 'K':
	case 'k':
		ret <<= 10;
		endptr++;
		fallthrough;
	default:
		break;
	}

	if (retptr)
		*retptr = endptr;

	return ret;
}
EXPORT_SYMBOL(memparse);

/**
 *	parse_option_str - Parse a string and check an option is set or not
 *	@str: String to be parsed
 *	@option: option name
 *
 *	This function parses a string containing a comma-separated list of
 *	strings like a=b,c.
 *
 *	Return true if there's such option in the string, or return false.
 */
bool parse_option_str(const char *str, const char *option)
{
	while (*str) {
		if (!strncmp(str, option, strlen(option))) {
			str += strlen(option);
			if (!*str || *str == ',')
				return true;
		}

		while (*str && *str != ',')
			str++;

		if (*str == ',')
			str++;
	}

	return false;
}

/*
 * Parse a string to get a param value pair.
 * You can use " around spaces, but can't escape ".
 * Hyphens and underscores equivalent in parameter names.
 */
/* IAMROOT, 2022.01.05:
 * - @args에서 'key=value' 형태의 arguments를 입력받아 parsing 한 뒤
 *   @param에 key, @val에 value를 저장한다.
 */
char *next_arg(char *args, char **param, char **val)
{
	unsigned int i, equals = 0;
	int in_quote = 0, quoted = 0;

	/* IAMROOT, 2024.10.15:
	 * - @args가 '"'라면 quote 안에 있는 것을 알리기 위해 flag를 설정한다.
	 */
	if (*args == '"') {
		args++;
		in_quote = 1;
		quoted = 1;
	}

	/* IAMROOT, 2024.10.15:
	 * - loop를 수행하여 '=' 문자를 찾아 해당 index를 equals에 저장한다.
	 */
	for (i = 0; args[i]; i++) {
		if (isspace(args[i]) && !in_quote)
			break;
		if (equals == 0) {
			if (args[i] == '=')
				equals = i;
		}
		if (args[i] == '"')
			in_quote = !in_quote;
	}

	/* IAMROOT, 2024.10.15:
	 * - 'key=value' 구조에서 'key' 부분을 @param에 저장한다.
	 */
	*param = args;
	/* IAMROOT, 2024.10.15:
	 * - '=' 문자가 없다면 value가 없는 것이므로 @val에 NULL을 저장한다.
	 */
	if (!equals)
		*val = NULL;
	else {
		/* IAMROOT, 2024.10.15:
		 * - '=' 문자를 찾았고 @param 처리를 위해 '='을 NULL로 변경한다.
		 */
		args[equals] = '\0';
		/* IAMROOT, 2024.10.15:
		 * - '=' 문자 index의 다음은 value이므로 @val에 저장한다.
		 */
		*val = args + equals + 1;

		/* Don't include quotes in value. */
		/* IAMROOT, 2024.10.15:
		 * - '"' 문자 및 empty string 예외 처리.
		 */
		if (**val == '"') {
			(*val)++;
			if (args[i-1] == '"')
				args[i-1] = '\0';
		}
	}
	if (quoted && args[i-1] == '"')
		args[i-1] = '\0';

	if (args[i]) {
		args[i] = '\0';
		args += i + 1;
	} else
		args += i;

	/* Chew up trailing spaces. */
	return skip_spaces(args);
}
EXPORT_SYMBOL(next_arg);
