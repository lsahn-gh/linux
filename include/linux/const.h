#ifndef _LINUX_CONST_H
#define _LINUX_CONST_H

#include <vdso/const.h>

/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 */
/*
 * IAMROOT, 2022.02.14:
 * @return x가 const일 경우 1, 아니면 0
 * - 참고 사이트
 *   https://stackoverflow.com/questions/49481217/linux-kernels-is-constexpr-macro
 * - VLA를 제거하기 위한 macro라고 한다.
 *
 * --- VLA(Variable-Length Array)
 *  가변 길이 배열(Variable-length array)은 프로그램 작성 시
 *  배열의 크기를 컴파일 타임에 정하지 않고,
 *  실행타임에 정할 수 있도록 하는 기능이다.
 *
 * ---
 *  삼항연산자에 의해 결국 2개의 유형만 존재하게 될것이다.
 *
 *  sizeof(int) == sizeof(*(int *)8) => true(1)
 *  sizeof(int) == sizeof(*(void *) 0) => false (0)
 *
 *  즉 int *이냐 void *라고 오는게 여기서의 핵심문제가 된다.
 *
 * 일단 3항연산자를 if문으로 풀어본다.
 *
 * if (8)
 *  return (void *)((long)(x) * 0);
 * else
 *  return (int *)8;
 *
 * --- x는 상수인경우
 * - An integer constant expression with the value 0, or such an expression
 * cast to type void *, is called a null pointer constant.
 *
 * x가 const일 경우 (void *)((long)(x) * 0l))는 위 정의에 의해 void *가된다.
 * 그리고 우린 이걸 NULL이라고 부른다.
 *
 * if (8)
 *  return (void *)0;
 * else
 *  return (int *)8;
 *
 * 그런데 여기서 return을 2개의 type으로한다. 한족은 void *, 한쪽은 int *
 * 그런데 gcc 표준에 따르면 한쪽이 void *일경우, 해당 type은 다른쪽의 type을
 * 따른다고 한다.
 * - [...] if one operand is a null pointer constant,
 *   the result has the type of the other operand; otherwise,
 *   one operand is a pointer to void or a qualified version of void,
 *   in which case the result type is a pointer to an appropriately qualified
 *   version of void.
 *
 * if (8)
 *  return (int *)0;
 * else
 *  return (int *)8;
 *
 * 이렇게되서 sizeof((int *)0)가 되고 결국엔 sizeof(int) == sizeof(*(int *)0)
 * 가되어 true가 된다.
 *
 * --- x가 상수가 아닌 경우(변수)
 *
 * if (8)
 *  return (void *)(x * 0);
 * else
 *  return (int *)8;
 *
 *  이 상황이 되는데, 여기서 x * 0은 0이 되긴해서 결과적으로 x가 상수일때랑
 *  똑같은것처럼 나오게 되지만 변수(x) * 상수(0) 이기 때문에
 *  (void *)0(변수)가 된다.
 *
 * if (8)
 *  return (void *)(0); // 변수인 void *
 * else
 *  return (int *)8;
 *
 * 즉 상수가 아닌 (void *)0이 결과가 되므로 sizeof(int) != sizeof(*(void *)0)
 * 가 된다. (sizeof(void) == 1)
 * 즉 false가 된다.
 */
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

#endif /* _LINUX_CONST_H */
