// SPDX-License-Identifier: GPL-2.0
/*
 * Extra Boot Config
 * Masami Hiramatsu <mhiramat@kernel.org>
 */

#define pr_fmt(fmt)    "bootconfig: " fmt

#include <linux/bootconfig.h>
#include <linux/bug.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/printk.h>
#include <linux/string.h>

/*
 * Extra Boot Config (XBC) is given as tree-structured ascii text of
 * key-value pairs on memory.
 * xbc_parse() parses the text to build a simple tree. Each tree node is
 * simply a key word or a value. A key node may have a next key node or/and
 * a child node (both key and value). A value node may have a next value
 * node (for array).
 */

/*
 * IAMROOT, 2022.01.04:
 */
static struct xbc_node *xbc_nodes __initdata;
/*
 * IAMROOT, 2022.01.04:
 * node가 한개씩 추가 될때마다 증가한다. XBC_NODE_MAX가 max개.
 */
static int xbc_node_num __initdata;
/*
 * IAMROOT, 2022.01.04:
 * - xbc_init 에서 memblock에 할당된 bootconfig string 주소로 초기화된다.
 * - xbc_data_size는 strlen(xbc_data) + 1 로 초기화된다.
 */
static char *xbc_data __initdata;
static size_t xbc_data_size __initdata;
/*
 * IAMROOT, 2022.01.04:
 * - key나 value의 node가 연쇄적으로 생성될때 직전 parent를 기억하기 위해
 *   사용한다.
 */
static struct xbc_node *last_parent __initdata;
static const char *xbc_err_msg __initdata;
/*
 * IAMROOT, 2022.01.04:
 * - error가 발생한 postion을 저장한다. error 발생시 return되어 사용된다.
 */
static int xbc_err_pos __initdata;
/*
 * IAMROOT, 2022.01.04:
 * - bracket 처리를 할때 보조용으로 사용한다.
 */
static int open_brace[XBC_DEPTH_MAX] __initdata;
static int brace_index __initdata;

/*
 * IAMROOT, 2022.01.04:
 * - errorr가 발생한 postion을 저장한다.
 */
static int __init xbc_parse_error(const char *msg, const char *p)
{
	xbc_err_msg = msg;
	xbc_err_pos = (int)(p - xbc_data);

	return -EINVAL;
}

/**
 * xbc_root_node() - Get the root node of extended boot config
 *
 * Return the address of root node of extended boot config. If the
 * extended boot config is not initiized, return NULL.
 */
struct xbc_node * __init xbc_root_node(void)
{
	if (unlikely(!xbc_data))
		return NULL;

	return xbc_nodes;
}

/**
 * xbc_node_index() - Get the index of XBC node
 * @node: A target node of getting index.
 *
 * Return the index number of @node in XBC node list.
 */
int __init xbc_node_index(struct xbc_node *node)
{
	return node - &xbc_nodes[0];
}

/**
 * xbc_node_get_parent() - Get the parent XBC node
 * @node: An XBC node.
 *
 * Return the parent node of @node. If the node is top node of the tree,
 * return NULL.
 */
struct xbc_node * __init xbc_node_get_parent(struct xbc_node *node)
{
	return node->parent == XBC_NODE_MAX ? NULL : &xbc_nodes[node->parent];
}

/**
 * xbc_node_get_child() - Get the child XBC node
 * @node: An XBC node.
 *
 * Return the first child node of @node. If the node has no child, return
 * NULL.
 */
struct xbc_node * __init xbc_node_get_child(struct xbc_node *node)
{
	return node->child ? &xbc_nodes[node->child] : NULL;
}

/**
 * xbc_node_get_next() - Get the next sibling XBC node
 * @node: An XBC node.
 *
 * Return the NEXT sibling node of @node. If the node has no next sibling,
 * return NULL. Note that even if this returns NULL, it doesn't mean @node
 * has no siblings. (You also has to check whether the parent's child node
 * is @node or not.)
 */
struct xbc_node * __init xbc_node_get_next(struct xbc_node *node)
{
	return node->next ? &xbc_nodes[node->next] : NULL;
}

/**
 * xbc_node_get_data() - Get the data of XBC node
 * @node: An XBC node.
 *
 * Return the data (which is always a null terminated string) of @node.
 * If the node has invalid data, warn and return NULL.
 */
const char * __init xbc_node_get_data(struct xbc_node *node)
{
	int offset = node->data & ~XBC_VALUE;

	if (WARN_ON(offset >= xbc_data_size))
		return NULL;

	return xbc_data + offset;
}

/*
 * IAMROOT, 2022.01.04:
 * - node에 저장된 data와 prefix를 비교한다.
 * ex) prefix = abc.123.456, node data = abc일 경우
 *     prefix가 123.456으로 갱신되면서 true.
 *     그 다음 해당 함수가 호출됬을대 node data가 123이면
 *     prefix가 456d으로 갱신.
 */
static bool __init
xbc_node_match_prefix(struct xbc_node *node, const char **prefix)
{
	const char *p = xbc_node_get_data(node);
	int len = strlen(p);

	if (strncmp(*prefix, p, len))
		return false;

	p = *prefix + len;
	if (*p == '.')
		p++;
	else if (*p != '\0')
		return false;
	*prefix = p;

	return true;
}

/**
 * xbc_node_find_subkey() - Find a subkey node which matches given key
 * @parent: An XBC node.
 * @key: A key string.
 *
 * Search a key node under @parent which matches @key. The @key can contain
 * several words jointed with '.'. If @parent is NULL, this searches the
 * node from whole tree. Return NULL if no node is matched.
 */
/*
 * IAMROOT, 2022.01.05:
 * - key에 해당하는 node를 탐색한다. key가 123.456.789 같은 값이면
 *   최정적으로 789 node를 return할것이다.
 */
struct xbc_node * __init
xbc_node_find_subkey(struct xbc_node *parent, const char *key)
{
	struct xbc_node *node;

	if (parent)
		node = xbc_node_get_subkey(parent);
	else
		node = xbc_root_node();

	while (node && xbc_node_is_key(node)) {
/*
 * IAMROOT, 2022.01.05:
 * - 해당 node에서 탐색이 실패한경우 sibling을 탐색한다.
 * - 성공했는데 key가 아직 null이 아닌 경우 key가 abc.123 과 같은
 *   연쇄 값이므로 탐색이 아직 안끝낫다.
 */
		if (!xbc_node_match_prefix(node, &key))
			node = xbc_node_get_next(node);
		else if (*key != '\0')
			node = xbc_node_get_subkey(node);
		else
			break;
	}

	return node;
}

/**
 * xbc_node_find_value() - Find a value node which matches given key
 * @parent: An XBC node.
 * @key: A key string.
 * @vnode: A container pointer of found XBC node.
 *
 * Search a value node under @parent whose (parent) key node matches @key,
 * store it in *@vnode, and returns the value string.
 * The @key can contain several words jointed with '.'. If @parent is NULL,
 * this searches the node from whole tree. Return the value string if a
 * matched key found, return NULL if no node is matched.
 * Note that this returns 0-length string and stores NULL in *@vnode if the
 * key has no value. And also it will return the value of the first entry if
 * the value is an array.
 */
const char * __init
xbc_node_find_value(struct xbc_node *parent, const char *key,
		    struct xbc_node **vnode)
{
	struct xbc_node *node = xbc_node_find_subkey(parent, key);

	if (!node || !xbc_node_is_key(node))
		return NULL;

	node = xbc_node_get_child(node);
	if (node && !xbc_node_is_value(node))
		return NULL;

	if (vnode)
		*vnode = node;

	return node ? xbc_node_get_data(node) : "";
}

/**
 * xbc_node_compose_key_after() - Compose partial key string of the XBC node
 * @root: Root XBC node
 * @node: Target XBC node.
 * @buf: A buffer to store the key.
 * @size: The size of the @buf.
 *
 * Compose the partial key of the @node into @buf, which is starting right
 * after @root (@root is not included.) If @root is NULL, this returns full
 * key words of @node.
 * Returns the total length of the key stored in @buf. Returns -EINVAL
 * if @node is NULL or @root is not the ancestor of @node or @root is @node,
 * or returns -ERANGE if the key depth is deeper than max depth.
 * This is expected to be used with xbc_find_node() to list up all (child)
 * keys under given key.
 */
/*
 * IAMROOT, 2022.01.05:
 * - node는 key의 마지막 node일 것이다(key = 123.456.789였다면 789 node)
 * - parent로 역순으로 올라가 최종적으로 key를 완성하기 위함이다.
 *   keys에 저장해놨다가 역순으로 while을 돌며 완성하는 개념.
 */
int __init xbc_node_compose_key_after(struct xbc_node *root,
				      struct xbc_node *node,
				      char *buf, size_t size)
{
	u16 keys[XBC_DEPTH_MAX];
	int depth = 0, ret = 0, total = 0;

	if (!node || node == root)
		return -EINVAL;

	if (xbc_node_is_value(node))
		node = xbc_node_get_parent(node);

	while (node && node != root) {
		keys[depth++] = xbc_node_index(node);
		if (depth == XBC_DEPTH_MAX)
			return -ERANGE;
		node = xbc_node_get_parent(node);
	}
	if (!node && root)
		return -EINVAL;

	while (--depth >= 0) {
		node = xbc_nodes + keys[depth];
		ret = snprintf(buf, size, "%s%s", xbc_node_get_data(node),
			       depth ? "." : "");
		if (ret < 0)
			return ret;
		if (ret > size) {
			size = 0;
		} else {
			size -= ret;
			buf += ret;
		}
		total += ret;
	}

	return total;
}

/**
 * xbc_node_find_next_leaf() - Find the next leaf node under given node
 * @root: An XBC root node
 * @node: An XBC node which starts from.
 *
 * Search the next leaf node (which means the terminal key node) of @node
 * under @root node (including @root node itself).
 * Return the next node or NULL if next leaf node is not found.
 */
/*
 * IAMROOT, 2022.01.05:
 * - root부터 시작해 leaf를찾는다.
 * - leaf는 key의 마지막 node를 의미한다.
 */
struct xbc_node * __init xbc_node_find_next_leaf(struct xbc_node *root,
						 struct xbc_node *node)
{
	struct xbc_node *next;

	if (unlikely(!xbc_data))
		return NULL;

	if (!node) {	/* First try */
		node = root;
		if (!node)
			node = xbc_nodes;
	} else {
/*
 * IAMROOT, 2022.01.05:
 * - subkey를 뒤져본다.
 */
		/* Leaf node may have a subkey */
		next = xbc_node_get_subkey(node);
		if (next) {
			node = next;
			goto found;
		}

		if (node == root)	/* @root was a leaf, no child node. */
			return NULL;

/*
 * IAMROOT, 2022.01.05:
 * - 현재 node의 subkey에도 없으면 parent로 역순으로 올라가 찾아본다.
 */
		while (!node->next) {
			node = xbc_node_get_parent(node);
			if (node == root)
				return NULL;
			/* User passed a node which is not uder parent */
			if (WARN_ON(!node))
				return NULL;
		}
		node = xbc_node_get_next(node);
	}

found:
	while (node && !xbc_node_is_leaf(node))
		node = xbc_node_get_child(node);

	return node;
}

/**
 * xbc_node_find_next_key_value() - Find the next key-value pair nodes
 * @root: An XBC root node
 * @leaf: A container pointer of XBC node which starts from.
 *
 * Search the next leaf node (which means the terminal key node) of *@leaf
 * under @root node. Returns the value and update *@leaf if next leaf node
 * is found, or NULL if no next leaf node is found.
 * Note that this returns 0-length string if the key has no value, or
 * the value of the first entry if the value is an array.
 */
/*
 * IAMROOT, 2022.01.05:
 * - leaf를 찾고 leaf에 해당하는 value data를 return한다.
 */
const char * __init xbc_node_find_next_key_value(struct xbc_node *root,
						 struct xbc_node **leaf)
{
	/* tip must be passed */
	if (WARN_ON(!leaf))
		return NULL;

	*leaf = xbc_node_find_next_leaf(root, *leaf);
	if (!*leaf)
		return NULL;
	if ((*leaf)->child)
		return xbc_node_get_data(xbc_node_get_child(*leaf));
	else
		return "";	/* No value key */
}

/* XBC parse and tree build */
/* IAMROOT, 2024.10.20:
 * - @node에 @data, @flag를 저장한다.
 */
static int __init xbc_init_node(struct xbc_node *node, char *data, u32 flag)
{
	unsigned long offset = data - xbc_data;

	if (WARN_ON(offset >= XBC_DATA_MAX))
		return -EINVAL;

	node->data = (u16)offset | flag;
	node->child = 0;
	node->next = 0;

	return 0;
}

/* IAMROOT, 2022.01.04:
 * - struct xbc_node를 하나 할당하고 @data, @flag로 초기화한다.
 */
static struct xbc_node * __init xbc_add_node(char *data, u32 flag)
{
	struct xbc_node *node;

	/* IAMROOT, 2024.10.20:
	 * - xbc_nodes가 꽉 찼다면 더 이상 추가할 수 없으므로 null을 반환한다.
	 */
	if (xbc_node_num == XBC_NODE_MAX)
		return NULL;

	/* IAMROOT, 2024.10.20:
	 * - node는 xbc_nodes에서 가져오며 해당 array는 build time에 크기가
	 *   결정된다.
	 */
	node = &xbc_nodes[xbc_node_num++];
	if (xbc_init_node(node, data, flag) < 0)
		return NULL;

	return node;
}

static inline __init struct xbc_node *xbc_last_sibling(struct xbc_node *node)
{
	while (node->next)
		node = xbc_node_get_next(node);

	return node;
}

static inline __init struct xbc_node *xbc_last_child(struct xbc_node *node)
{
	while (node->child)
		node = xbc_node_get_child(node);

	return node;
}

/*
 * IAMROOT, 2022.01.04:
 * ps) last_parent node를 lpnode, 추가 node를 Nnode 라고 명명
 * - lpnode 가 없는경우
 *   before            after
 *   -------------------------------
 *   node1 -> ..       node1 -> ..
 *    |(sibling)        |(sibling)
 *   node2 -> ..       node2 -> ..
 *                      |(sibling)
 *                     Nnode
 *                  
 *   현재 node가 최초의 node가 되므로 parent는 없다는 의미로 XBC_NODE_MAX
 *   로 설정한다.
 *   최초의 node끼리는 sibling(next)으로 연결된다.
 *
 * - lpnode 가 있고 lpnode의 child가 없는경우 or
 *   head로 강제 설정인 경우
 *   lpnode와 부모자식으로 연결한다. last_parent의 자식을
 *   sibling으로 연결한다
 *
 *   before                         after
 *   ----------------------------------------
 *         (child)                   (child)
 *   lpnode ------> (NULL)     lpnode ------> Nnode 
 *   lpnode ------> cnode      lpnode ------> Nnode -> cnode
 *
 * - lpnode 가 있고 lpnode의 child가 있는경우
 *   lpnode의 cnode의 제일 마지막 snode에 추가한ㄷ.
 *   before                         after
 *   ----------------------------------------
 *         (child)                   (child)
 *   lpnode ------> cnode     lpnode ------> cnode 
 *                    | (sibling)              |
 *                  snode                    snode
 *                    |                        |
 *                  (null)                   nNode
 *
 */
static struct xbc_node * __init __xbc_add_sibling(char *data, u32 flag, bool head)
{
	struct xbc_node *sib, *node = xbc_add_node(data, flag);

	/* IAMROOT, 2024.10.20:
	 * - struct xbc_node 할당 성공시 처리.
	 */
	if (node) {
		if (!last_parent) {
			/* Ignore @head in this case */
			node->parent = XBC_NODE_MAX;
			sib = xbc_last_sibling(xbc_nodes);
			sib->next = xbc_node_index(node);
		} else {
			node->parent = xbc_node_index(last_parent);
			if (!last_parent->child || head) {
				node->next = last_parent->child;
				last_parent->child = xbc_node_index(node);
			} else {
				sib = xbc_node_get_child(last_parent);
				sib = xbc_last_sibling(sib);
				sib->next = xbc_node_index(node);
			}
		}
	} else
		xbc_parse_error("Too many nodes", data);

	return node;
}

/*
 * IAMROOT, 2022.01.04:
 * - lpnode의 child의 last sibling에 위치시킨다.
 */
static inline struct xbc_node * __init xbc_add_sibling(char *data, u32 flag)
{
	return __xbc_add_sibling(data, flag, false);
}

/*
 * IAMROOT, 2022.01.04:
 * - lpnode의 child가 된다.(head sibling)
 */
static inline struct xbc_node * __init xbc_add_head_sibling(char *data, u32 flag)
{
	return __xbc_add_sibling(data, flag, true);
}

/*
 * IAMROOT, 2022.01.04:
 * - lpnode의 child의 마지막 sibling에 추가한다. lpnode를 생성된 node로
 *   업데이트한다.
 */
static inline __init struct xbc_node *xbc_add_child(char *data, u32 flag)
{
	struct xbc_node *node = xbc_add_sibling(data, flag);

	if (node)
		last_parent = node;

	return node;
}

/* IAMROOT, 2022.01.04:
 * - alpha/num, '-', '_'만 허용하고 나머지는 허용하지 않음.
 */
static inline __init bool xbc_valid_keyword(char *key)
{
	if (key[0] == '\0')
		return false;

	while (isalnum(*key) || *key == '-' || *key == '_')
		key++;

	return *key == '\0';
}

/*
 * IAMROOT, 2022.01.04:
 * - #로 시작했던 문자열은 comment처리한다.
 */
static char *skip_comment(char *p)
{
	char *ret;

	ret = strchr(p, '\n');
	if (!ret)
		ret = p + strlen(p);
	else
		ret++;

	return ret;
}

static char *skip_spaces_until_newline(char *p)
{
	while (isspace(*p) && *p != '\n')
		p++;
	return p;
}

/*
 * IAMROOT, 2022.01.04:
 * - bracket 진입전 last_parent를 저장해놓는다. depth검사도 수행한다.
 */
static int __init __xbc_open_brace(char *p)
{
	/* Push the last key as open brace */
	open_brace[brace_index++] = xbc_node_index(last_parent);
	if (brace_index >= XBC_DEPTH_MAX)
		return xbc_parse_error("Exceed max depth of braces", p);

	return 0;
}

/*
 * IAMROOT, 2022.01.04:
 * - bracket의 {, }의 쌍이 맞는지 검사하고 last_parent를 depth에 맞게
 *   갱신한다.
 */
static int __init __xbc_close_brace(char *p)
{
	brace_index--;
	if (!last_parent || brace_index < 0 ||
	    (open_brace[brace_index] != xbc_node_index(last_parent)))
		return xbc_parse_error("Unexpected closing brace", p);

	if (brace_index == 0)
		last_parent = NULL;
	else
		last_parent = &xbc_nodes[open_brace[brace_index - 1]];

	return 0;
}

/*
 * Return delimiter or error, no node added. As same as lib/cmdline.c,
 * you can use " around spaces, but can't escape " for value.
 */
/*
 * IAMROOT, 2022.01.04:
 * @__v value string. parse되면 '\0'이 붙어 trim이 될것이다.
 * @__n next value pos. trim된 __v의 다음 위치를 가리킬것이다.
 * @return c ,;\n#}중 하나. 
 */
static int __init __xbc_parse_value(char **__v, char **__n)
{
	char *p, *v = *__v;
	int c, quotes = 0;

	v = skip_spaces(v);
	while (*v == '#') {
		v = skip_comment(v);
		v = skip_spaces(v);
	}
	if (*v == '"' || *v == '\'') {
		quotes = *v;
		v++;
	}
	p = v - 1;
	while ((c = *++p)) {
		if (!isprint(c) && !isspace(c))
			return xbc_parse_error("Non printable value", p);
		if (quotes) {
			if (c != quotes)
				continue;
			quotes = 0;
			*p++ = '\0';
			p = skip_spaces_until_newline(p);
			c = *p;
			if (c && !strchr(",;\n#}", c))
				return xbc_parse_error("No value delimiter", p);
			if (*p)
				p++;
			break;
		}
		if (strchr(",;\n#}", c)) {
			*p++ = '\0';
			v = strim(v);
			break;
		}
	}
	if (quotes)
		return xbc_parse_error("No closing quotes", p);
	if (c == '#') {
		p = skip_comment(p);
		c = '\n';	/* A comment must be treated as a newline */
	}
	*__n = p;
	*__v = v;

	return c;
}

/*
 * IAMROOT, 2022.01.04:
 * - value들을 child node로 만들어 연결한다.
 *   x = v1, v2, v3일경우
 *         (child)         (child)        (child)   
 *  x_node ------> v1_node ------> v2_node -----> v3_node
 *  (XBC_KEY)     (XBC_VALUE)     (XBC_VALUE)     (XBC_VALUE)
 */
static int __init xbc_parse_array(char **__v)
{
	struct xbc_node *node;
	char *next;
	int c = 0;

/*
 * IAMROOT, 2022.01.04:
 * - lpnode가 존재하면 lpnode의 cnode를 lpnode로 갱신한다.
 */
	if (last_parent->child)
		last_parent = xbc_node_get_child(last_parent);

	do {
		c = __xbc_parse_value(__v, &next);
		if (c < 0)
			return c;

		node = xbc_add_child(*__v, XBC_VALUE);
		if (!node)
			return -ENOMEM;
		*__v = next;
	} while (c == ',');
	node->child = 0;

	return c;
}

static inline __init
struct xbc_node *find_match_node(struct xbc_node *node, char *k)
{
	while (node) {
		if (!strcmp(xbc_node_get_data(node), k))
			break;
		node = xbc_node_get_next(node);
	}
	return node;
}

/*
 * IAMROOT, 2022.01.04:
 * - lpnode에서 시작해서 해당 key와 일치하는 node를 찾는다. 없는경우
 *   node를 새로 생성한다.
 * - node가 새로 생성하거나 검색이 된경우 lpnode를 해당 node로 갱신한다.
 *
 * - 새로 node가 생성되는 경우엔 lpnode의 child의 제일 마지막 sibling이
 *   된다.
 *
 * -------------------- (상세)
 * - lpnode가 없는 경우
 *   key값과 일치하는 node를 찾는다. 찾은경우 lpnode를 해당 node로
 *   설정하고 못찾았으면 node를 생성한다.
 * - lpnode가 있는 있는 경우.
 *   이미 상위에서 tree를 타고 왔다는 것을 의미 한다.
 *   
 * --- lpnode의 child가 없는경우
 *     node를 생성하고 해당 node를 lpnode의 child로 한다.
 * --- lpnode의 child가 있으면서 해당 cnode가 value인경우
 *     cnode의 sibling에서 find_match_node를 시작한다.
 * --- 그 외의 경우
 *     lpnode의 child에서 find_match_node를 시작한다.
 *     
 * node가 생성되거나 찾아지면 lpnode는 해당 node로 업데이트될것이다.
 */
static int __init __xbc_add_key(char *k)
{
	struct xbc_node *node, *child;

	/* IAMROOT, 2024.10.20:
	 * - @k(ey) validation 확인.
	 */
	if (!xbc_valid_keyword(k))
		return xbc_parse_error("Invalid keyword", k);

	/* IAMROOT, 2024.10.20:
	 * - xbc_node가 하나도 없는 경우 parent 탐색없이 바로 add로 넘어간다.
	 */
	if (unlikely(xbc_node_num == 0))
		goto add_node;

	if (!last_parent)	/* the first level */
		node = find_match_node(xbc_nodes, k);
	else {
		child = xbc_node_get_child(last_parent);
		/* Since the value node is the first child, skip it. */
/*
 * IAMROOT, 2022.01.05:
 * - 연속 key를 child로 두어야되는데 그전에 이미 child가 value인게
 *   이미 있다면 value node의 sibling에 key node를 생성한다.
 *   그렇기 때문에 value를 만나면 sibling에 key node를 만들것이다.
 */
		if (child && xbc_node_is_value(child))
			child = xbc_node_get_next(child);
		node = find_match_node(child, k);
	}

	if (node)
		last_parent = node;
	else {
add_node:
		node = xbc_add_child(k, XBC_KEY);
		if (!node)
			return -ENOMEM;
	}
	return 0;
}

/* IAMROOT, 2022.01.04:
 * - @k(ey)를 포맷에 맞춰 tokenizing하고 추가한다.
 */
static int __init __xbc_parse_keys(char *k)
{
	char *p;
	int ret;

	k = strim(k);

	/* IAMROOT, 2024.10.24:
	 * - @k(ey)를 '.' delim에 따라 tokenizing하고 add_key(..)를 호출한다.
	 *
	 *   예) @k: ab.bc.cd
	 */
	while ((p = strchr(k, '.'))) {
		*p++ = '\0';
		ret = __xbc_add_key(k);
		if (ret)
			return ret;
		k = p;
	}

	/* IAMROOT, 2024.10.24:
	 * - 마지막 key 추가.
	 */
	return __xbc_add_key(k);
}

/*
 * IAMROOT, 2022.01.04:
 * ps) last_parent node를 lpnode라고 명명
 * - last_parent를 이용해, 원래 last_parent를 prev_parent로 저장해놓고
 *   last_parent로 마지막 parent를 기억하며 add를 하고 작업이 다끝나면
 *   prev_parent로 last_parent를 복구한다.
 */
static int __init xbc_parse_kv(char **k, char *v, int op)
{
	struct xbc_node *prev_parent = last_parent;
	struct xbc_node *child;
	char *next;
	int c, ret;

	/* IAMROOT, 2024.10.24:
	 * - @k(ey)를 parsing 한다.
	 */
	ret = __xbc_parse_keys(*k);
	if (ret)
		return ret;

	/* IAMROOT, 2024.10.24:
	 * - @v(alue)를 parsing 한다.
	 */
	c = __xbc_parse_value(&v, &next);
	if (c < 0)
		return c;

/*
 * IAMROOT, 2022.01.04:
 * ex) a1.a2.a3 의 param일 경우 last_parent는 a3 node를 가리키고 있을것이다.
 * 이경우 child가 존재하고, 해당 node가 value일 경우 이미 해당 key에 대한
 * 값이 존재하는 경우이다.
 * op가 :=인 경우는 대체 되므로 현재 진입한 vavlue(v)로 재초기화를 수행하고,
 * +=인 경우는 value가 추가 되는 개념이라 head child로 추가한다.
 */
	child = xbc_node_get_child(last_parent);
	if (child && xbc_node_is_value(child)) {

		if (op == '=')
			return xbc_parse_error("Value is redefined", v);
		if (op == ':') {
			unsigned short nidx = child->next;

			xbc_init_node(child, v, XBC_VALUE);
			child->next = nidx;	/* keep subkeys */
			goto array;
		}
		/* op must be '+' */
		last_parent = xbc_last_child(child);
	}
	/* The value node should always be the first child */
	if (!xbc_add_head_sibling(v, XBC_VALUE))
		return -ENOMEM;

array:
	if (c == ',') {	/* Array */
		c = xbc_parse_array(&next);
		if (c < 0)
			return c;
	}

/*
 * IAMROOT, 2022.01.04:
 * - 복귀하기 위한 준비. 함수 진입전 last_parent로 원복하고 c가 bracket일
 *   경우 검사를 수행한다.
 */
	last_parent = prev_parent;

	if (c == '}') {
		ret = __xbc_close_brace(next - 1);
		if (ret < 0)
			return ret;
	}

/*
 * IAMROOT, 2022.01.04:
 * - value parse가 끝난 주소로 갱신한다.
 */
	*k = next;

	return 0;
}

/*
 * IAMROOT, 2022.01.04:
 * @param pos
 * @n '\n'이나 '}'. param의 마지막 주소
 *
 * 여러개의 key로 이루어진 param이 올 경우 last_parent로 가장 최근의
 * parent를 기억해서 연속으로 add를 하는 방식을 사용한다.
 *
 * last_parent를 다 사용하고 나면 prev_parent를 사용해
 * 해당 함수 진입전으로 last_parent을 복구한다.
 */
static int __init xbc_parse_key(char **k, char *n)
{
	struct xbc_node *prev_parent = last_parent;
	int ret;

	*k = strim(*k);
	if (**k != '\0') {
		ret = __xbc_parse_keys(*k);
		if (ret)
			return ret;
		last_parent = prev_parent;
	}
	*k = n;

	return 0;
}

/*
 * IAMROOT, 2022.01.04:
 * - key parse 및 add를 수행하고 bracket open처리를 한다.
 */
static int __init xbc_open_brace(char **k, char *n)
{
	int ret;

	ret = __xbc_parse_keys(*k);
	if (ret)
		return ret;
	*k = n;

	return __xbc_open_brace(n - 1);
}

/*
 * IAMROOT, 2022.01.04:
 * - key parse 및 add를 수행하고 bracket close처리를 한다.
 */
static int __init xbc_close_brace(char **k, char *n)
{
	int ret;

	ret = xbc_parse_key(k, n);
	if (ret)
		return ret;
	/* k is updated in xbc_parse_key() */

	return __xbc_close_brace(n - 1);
}

/*
 * IAMROOT, 2022.01.04:
 * - bracket이 짝이 맞는지, key length, depth등을 검사한다.
 */
static int __init xbc_verify_tree(void)
{
	int i, depth, len, wlen;
	struct xbc_node *n, *m;

/*
 * IAMROOT, 2022.01.04:
 * - closed bracket이 부족한경우.
 */
	/* Brace closing */
	if (brace_index) {
		n = &xbc_nodes[open_brace[brace_index]];
		return xbc_parse_error("Brace is not closed",
					xbc_node_get_data(n));
	}

	/* Empty tree */
	if (xbc_node_num == 0) {
		xbc_parse_error("Empty config", xbc_data);
		return -ENOENT;
	}

	for (i = 0; i < xbc_node_num; i++) {
		if (xbc_nodes[i].next > xbc_node_num) {
			return xbc_parse_error("No closing brace",
				xbc_node_get_data(xbc_nodes + i));
		}
	}

	/* Key tree limitation check */
	n = &xbc_nodes[0];
	depth = 1;
	len = 0;

/*
 * IAMROOT, 2022.01.04:
 * - key length, depth 길이, 를 검사.
 */
	while (n) {
		wlen = strlen(xbc_node_get_data(n)) + 1;
		len += wlen;
		if (len > XBC_KEYLEN_MAX)
			return xbc_parse_error("Too long key length",
				xbc_node_get_data(n));

		m = xbc_node_get_child(n);
		if (m && xbc_node_is_key(m)) {
			n = m;
			depth++;
			if (depth > XBC_DEPTH_MAX)
				return xbc_parse_error("Too many key words",
						xbc_node_get_data(n));
			continue;
		}
		len -= wlen;
		m = xbc_node_get_next(n);
		while (!m) {
			n = xbc_node_get_parent(n);
			if (!n)
				break;
			len -= strlen(xbc_node_get_data(n)) + 1;
			depth--;
			m = xbc_node_get_next(n);
		}
		n = m;
	}

	return 0;
}

/**
 * xbc_destroy_all() - Clean up all parsed bootconfig
 *
 * This clears all data structures of parsed bootconfig on memory.
 * If you need to reuse xbc_init() with new boot config, you can
 * use this.
 */
void __init xbc_destroy_all(void)
{
	xbc_data = NULL;
	xbc_data_size = 0;
	xbc_node_num = 0;
	memblock_free_ptr(xbc_nodes, sizeof(struct xbc_node) * XBC_NODE_MAX);
	xbc_nodes = NULL;
	brace_index = 0;
}

/**
 * xbc_init() - Parse given XBC file and build XBC internal tree
 * @buf: boot config text
 * @emsg: A pointer of const char * to store the error message
 * @epos: A pointer of int to store the error position
 *
 * This parses the boot config text in @buf. @buf must be a
 * null terminated string and smaller than XBC_DATA_MAX.
 * Return the number of stored nodes (>0) if succeeded, or -errno
 * if there is any error.
 * In error cases, @emsg will be updated with an error message and
 * @epos will be updated with the error position which is the byte offset
 * of @buf. If the error is not a parser error, @epos will be -1.
 */
/*
 * IAMROOT, 2022.01.04:
 * @buf in. bootconfig string이 있는 buffer
 * @emsg out. error 발생이유에 대한 string out
 * @epos out. error가 발생한 buf의 pos
 * @return result
 *
 * - Documentation/admin-guide/bootconfig.rst 참고
 *   해당 규칙에 따라 node를 만든다.
 * - xbc_nodes를 초기화 한다.
 *
 * ---
 * - param(keys)
 * param 은 여러개의 key로 이뤄질수있다.
 *
 * ex) foo = value1 => key는 foo
 *     foo.bar.baz = value1 => key는 foo, bar, baz
 *
 * key 한개당 xbc_node 한개가 되며, xbc_node에는 해당 key의 string 주소가
 * XBC_KEY flag와 or되어 data로 저장된다.
 *
 * 같은 param에서 나온 key끼리는 부모자식 관계를 형성한다.
 *
 * ex) foo.bar.baz 라는 param에서
 * foo -(child)-> bar -(child)-> baz
 *
 * - 서로다른 param끼리는 sibling(next)로 이어진다.
 *
 * ex) foo.bar.baz, abc.def 라는 param이 있다고 했을때
 *
 *   foo-(child)-> bar -(child)-> baz
 *    |
 *    (sibling)
 *    |
 *   abc-(child)-> def
 *  
 * - value
 * value도 param과 마찬가지로 여러개로 구성이 될수 있는데, array라고 부른다.
 *
 * 마지막 key node에서 그다음 child가 value라면 그다음부턴 무조건 value라고
 * 생각한다.
 *
 * ex) foo(KEY) -(child)-> bar(KEY) -(child)-> v1(VALUE) -> ....
 * v1이 value인게 감지되면 그 뒤에 있는 node들은 전부 value가 된다.
 *                                  
 * boot command의 operation에 따라 해당 key의 value가 추가되거나 새로 덮어
 * 써지거나 할수있다.)
 * op 
 *  = : 이 전에 해당 key에 대한 value가 들어가있다면 fail.
 * += : 이 전에 해당 key에 대한 value가 들어가있다면 제일 첫번째 child로 추가 
 * := : 이 전에 해당 key에 대한 value가 있다면 다 지우고 현재 value로 치환.
 *
 * - subkey
 * ex) foo.bar = v1 이라는 boot config가 node로 이미 만들어져있다고 한다.
 * 
 * foo->bar->v1(value)
 *
 * 만약 이때 foo.bar.baz = v2 boot config를 찾게되면 다음과 같이 추가된다.
 *
 * foo->bar->v1(value)
 *           |
 *        (sibling)
 *           |
 *          baz(key)->v2(value)
 *
 * xbc_node의 규칙상 마지막 key node의 다음은 무조건 value여야 되는데
 * 위와 같은 상황일 경우는 첫번째 value의 sibling을 subkey라는 개념으로
 * 고려해 위와 같이 추가한다.
 *
 * 그래서 node를 add하거나 search할때 위의 subkey를 고려하여 첫번째 value에
 * 대해서는 sibling을 확인하는것이 source에서 보인다.
 */

int __init xbc_init(char *buf, const char **emsg, int *epos)
{
	char *p, *q;
	int ret, c;

	if (epos)
		*epos = -1;

	if (xbc_data) {
		if (emsg)
			*emsg = "Bootconfig is already initialized";
		return -EBUSY;
	}

	ret = strlen(buf);
	if (ret > XBC_DATA_MAX - 1 || ret == 0) {
		if (emsg)
			*emsg = ret ? "Config data is too big" :
				"Config data is empty";
		return -ERANGE;
	}

	/* IAMROOT, 2024.10.21:
	 * - XBC_NODE_MAX만큼의 struct xbc_node를 저장할 수 있는 메모리를
	 *   할당한다.
	 */
	xbc_nodes = memblock_alloc(sizeof(struct xbc_node) * XBC_NODE_MAX,
				   SMP_CACHE_BYTES);
	if (!xbc_nodes) {
		if (emsg)
			*emsg = "Failed to allocate bootconfig nodes";
		return -ENOMEM;
	}
	memset(xbc_nodes, 0, sizeof(struct xbc_node) * XBC_NODE_MAX);
	xbc_data = buf;
	xbc_data_size = ret + 1;
	last_parent = NULL;

	/* IAMROOT, 2024.10.21:
	 * - @buf(bootconfig data)를 tokenizing 하여 parsing 한다.
	 */
	p = buf;
	do {
		/* IAMROOT, 2024.10.21:
		 * - delimiter("{}=+;:\n#")를 기준으로 token를 분리한다.
		 */
		q = strpbrk(p, "{}=+;:\n#");
		if (!q) {
			/* IAMROOT, 2024.10.21:
			 * - 마지막 token이므로 parsing을 끝낸다.
			 */
			p = skip_spaces(p);
			if (*p != '\0')
				ret = xbc_parse_error("No delimiter", p);
			break;
		}

		/* IAMROOT, 2022.01.04:
		 * - delimiter를 '\0'로 교체한다.
		 *
		 *   예) p = "123{456"
		 *       c = q = '{'
		 *       -------------
		 *       p = "123'\0'456"
		 */
		c = *q;
		*q++ = '\0';

		/* IAMROOT, 2024.10.21:
		 * - delimiter에 따라 다르게 처리한다.
		 */
		switch (c) {
		case ':':
		case '+':
			/* IAMROOT, 2022.01.04:
			 * - 아래 2개의 token에 대해 처리.
			 *   1). ':='
			 *   2). '+='
			 */
			if (*q++ != '=') {
				ret = xbc_parse_error(c == '+' ?
						"Wrong '+' operator" :
						"Wrong ':' operator",
							q - 2);
				break;
			}
			fallthrough;
		case '=':
			/* IAMROOT, 2022.01.04:
			 * - kv를 parsing한다.
			 *
			 *   입력값 예) abc=123
			 *   ------------------
			 *              p = abc
			 *              q = 123
			 */
			ret = xbc_parse_kv(&p, q, c);
			break;
		case '{':
			ret = xbc_open_brace(&p, q);
			break;
		case '#':
			q = skip_comment(q);
			fallthrough;
		case ';':
		case '\n':
			ret = xbc_parse_key(&p, q);
			break;
		case '}':
			ret = xbc_close_brace(&p, q);
			break;
		}
	} while (!ret);

	if (!ret)
		ret = xbc_verify_tree();

	if (ret < 0) {
		if (epos)
			*epos = xbc_err_pos;
		if (emsg)
			*emsg = xbc_err_msg;
		xbc_destroy_all();
	} else
		ret = xbc_node_num;

	return ret;
}

/**
 * xbc_debug_dump() - Dump current XBC node list
 *
 * Dump the current XBC node list on printk buffer for debug.
 */
void __init xbc_debug_dump(void)
{
	int i;

	for (i = 0; i < xbc_node_num; i++) {
		pr_debug("[%d] %s (%s) .next=%d, .child=%d .parent=%d\n", i,
			xbc_node_get_data(xbc_nodes + i),
			xbc_node_is_value(xbc_nodes + i) ? "value" : "key",
			xbc_nodes[i].next, xbc_nodes[i].child,
			xbc_nodes[i].parent);
	}
}
