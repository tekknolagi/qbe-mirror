#include "../all.h"
#include "arm64.h"

typedef struct TClass Class;
typedef struct Insl Insl;

struct TClass {
	char inmem; /* 0, 1, or 2 for scalar argument on the stack */
	char ishfa;
	struct {
		char base;
		char size;
	} hfa;
	int align;
	uint64_t size;
};

struct Insl {
	Ins i;
	Insl *link;
};

static int
isfloatv(Typ *t, char *cls)
{
	Field *f;
	uint n;

	for (n=0; n<t->nunion; n++)
		for (f=t->fields[n]; f->type != FEnd; f++)
			switch (f->type) {
			case Fs:
				if (*cls == Kd)
					return 0;
				*cls = Ks;
				break;
			case Fd:
				if (*cls == Ks)
					return 0;
				*cls = Kd;
				break;
			case FTyp:
				if (isfloatv(&typ[f->len], cls))
					break;
			default:
				return 0;
			}
	return 1;
}

static void
typclass(Class *c, Typ *t)
{
	uint64_t sz;
	int al;

	sz = t->size;
	al = 1 << t->align;

	/* the ABI requires sizes to be rounded
	 * up to the nearest multiple of 8
	 */
	if (al < 8)
		al = 8;
	sz = (sz + al-1) & -al;

	c->size = sz;
	c->align = t->align;

	if (t->dark || sz > 16 || sz == 0) {
		/* large or unaligned structures are
		 * required to be passed in memory
		 */
		c->inmem = 1;
		return;
	}

	c->hfa.base = -1;
	c->ishfa = isfloatv(t, &c->hfa.base);
	if (c->hfa.base == Ks)
		c->hfa.size = t->size / 4;
	else
		c->hfa.size = t->size / 8;
}

static void
blit(Ref rstk, uint soff, Ref rsrc, uint sz, Fn *fn)
{
	Ref r, r1;
	uint boff;

	/* it's an impolite blit, we might go across the end
	 * of the source object a little bit... */
	for (boff=0; sz>0; sz-=8, soff+=8, boff+=8) {
		r = newtmp("abi", Kl, fn);
		r1 = newtmp("abi", Kl, fn);
		emit(Ostorel, 0, R, r, r1);
		emit(Oadd, Kl, r1, rstk, getcon(soff, fn));
		r1 = newtmp("abi", Kl, fn);
		emit(Oload, Kl, r, r1, R);
		emit(Oadd, Kl, r1, rsrc, getcon(boff, fn));
	}
}

#if 0
static int
retr(Ref reg[4], Class *cret)
{
	int n, ca;

	if (cret->ishfa) {
		ca = cret->hfa.size << 2;
		for (n=0; n<cret->hfa.size; n++)
			reg[n] = TMP(V0+n);
	} else {
		ca = cret->size / 8;
		reg[0] = TMP(R0);
		reg[1] = TMP(R1);
	}
	return ca;
}
#endif

static void
selret(Blk *b, Fn *fn)
{
	int i, j, k, s, ca;
	Ref r, r0;
	Class cret;

	j = b->jmp.type;

	if (!isret(j) || j == Jret0)
		return;

	r0 = b->jmp.arg;
	b->jmp.type = Jret0;

	if (j == Jretc) {
		typclass(&cret, &typ[fn->retty]);
		if (cret.inmem) {
			assert(rtype(fn->retr) == RTmp);
			blit(fn->retr, 0, r0, cret.size, fn);
			ca = 0;
		} else if (cret.ishfa) {
			k = cret.hfa.base;
			s = 4 + 4 * (k == Kd);
			for (i=0; i<cret.hfa.size; i++) {
				r = newtmp("abi", k, fn);
				emit(Oload, k, TMP(V0+i), r, R);
				emit(Oadd, Kl, r, r0, getcon(i*s, fn));
			}
			ca = cret.hfa.size << 2;
		} else {
			if (cret.size > 8) {
				r = newtmp("abi", Kl, fn);
				emit(Oload, Kl, TMP(R1), r, R);
				emit(Oadd, Kl, r, r0, getcon(8, fn));
			}
			emit(Oload, Kl, TMP(R0), r0, R);
			ca = cret.size / 8;
		}
	} else {
		k = j - Jretw;
		if (KBASE(k) == 0) {
			emit(Ocopy, k, TMP(R0), r0, R);
			ca = 1;
		} else {
			emit(Ocopy, k, TMP(V0), r0, R);
			ca = 1 << 2;
		}
	}

	b->jmp.arg = CALL(ca);
}

static int
argsclass(Ins *i0, Ins *i1, Class *carg, Ref *env)
{
	int ngp, nfp, *pn;
	Class *c;
	Ins *i;

	ngp = 8;
	nfp = 8;
	for (i=i0, c=carg; i<i1; i++, c++)
		switch (i->op) {
		case Opar:
		case Oarg:
			if (KBASE(i->cls) == 0)
				pn = &ngp;
			else
				pn = &nfp;
			if (*pn > 0) {
				*pn -= 1;
				c->inmem = 0;
			} else
				c->inmem = 2;
			c->align = 3;
			c->size = 8;
			break;
		case Oparc:
		case Oargc:
			typclass(c, &typ[i->arg[0].val]);
			if (c->inmem)
				break;
			if (c->ishfa) {
				nfp -= c->hfa.size;
				pn = &nfp;
			} else {
				ngp -= c->size / 8;
				pn = &ngp;
			}
			if (*pn < 0) {
				*pn = 0;
				c->inmem = 1;
			}
			break;
		case Opare:
			*env = i->to;
			break;
		case Oarge:
			*env = i->arg[0];
			break;
		}

	return ((8-ngp) << 5) | ((8-nfp) << 9);
}

int a_rsave[] = {
	R0,  R1,  R2,  R3,  R4,  R5,  R6,  R7,
	R8,  R9,  R10, R11, R12, R13, R14, R15,
	IP0, IP1, R18,
	V0,  V1,  V2,  V3,  V4,  V5,  V6,  V7,
	V16, V17, V18, V19, V20, V21, V22, V23,
	V24, V25, V26, V27, V28, V29, V30,
	-1
};
int a_rclob[] = {
	R19, R20, R21, R22, R23, R24, R25, R26,
	R27, R28,
	V8,  V9,  V10, V11, V12, V13, V14, V15,
	-1
};
MAKESURE(arrays_ok,
	sizeof a_rsave == (NGPS+NFPS+1) * sizeof(int) &&
	sizeof a_rclob == (NCLR+1) * sizeof(int)
);

/* layout of call's second argument (RCall)
 *
 *  29   13    9    5   2  0
 *  |0.00|x|xxxx|xxxx|xxx|xx|                  range
 *        |    |    |   |  ` gp regs returned (0..2)
 *        |    |    |   ` fp regs returned    (0..4)
 *        |    |    ` gp regs passed          (0..8)
 *        |     ` fp regs passed              (0..8)
 *        ` is x8 used                        (0..1)
 */

bits
a_retregs(Ref r, int p[2])
{
	bits b;
	int ngp, nfp;

	assert(rtype(r) == RCall);
	ngp = r.val & 3;
	nfp = (r.val >> 2) & 7;
	if (p) {
		p[0] = ngp;
		p[1] = nfp;
	}
	b = 0;
	while (ngp--)
		b |= BIT(R0+ngp);
	while (nfp--)
		b |= BIT(V0+nfp);
	return b;
}

bits
a_argregs(Ref r, int p[2])
{
	bits b;
	int ngp, nfp, x8;

	assert(rtype(r) == RCall);
	ngp = (r.val >> 5) & 15;
	nfp = (r.val >> 9) & 15;
	x8 = (r.val >> 13) & 1;
	if (p) {
		p[0] = ngp + x8;
		p[1] = nfp;
	}
	b = 0;
	while (ngp--)
		b |= BIT(R0+ngp);
	while (nfp--)
		b |= BIT(V0+nfp);
	return b | ((bits)x8 << R8);
}

static Ref
rarg(int ty, int *ng, int *nf)
{
	if (KBASE(ty) == 0)
		return TMP(R0 + (*ng)++);
	else
		return TMP(V0 + (*nf)++);
}

static void
selcall(Fn *fn, Ins *i0, Ins *i1, Insl **ilp)
{
	Ins *i;
	Class *ca, *c, cret;
	int cty, ng, nf, al, varc, envc;
	uint64_t stk, off;
	Ref r, r1, r2, reg[2], env;
	Insl *il;

	env = R;
	ca = alloc((i1-i0) * sizeof ca[0]);

	cty = argsclass(i0, i1, ca, &env);
	for (stk=0, c=&ca[i1-i0]; c>ca;)
		if ((--c)->inmem) {
			if (c->align > 4)
				err("arm abi requires alignments of 16 or less");
			stk += c->size;
			if (c->align == 4)
				stk += stk & 15;
		}
	stk += stk & 15;
	if (stk) {
		r = getcon(stk, fn);
		emit(Oadd, Kl, TMP(SP), TMP(SP), r);
	}

	if (!req(i1->arg[1], R)) {
		typclass(&cret, &typ[i1->arg[1].val]);
		if (!cret.inmem) {
			/* get shit out of registers, shit might
			 * be in 1 or 2 gp regs, or up to 4 fp
			 * regs.
			if (cret.size > 8) {
				r = newtmp("abi", Kl, fn);
				cret.ref[1] = newtmp("abi", cret.cls[1], fn);
				emit(Ostorel, 0, R, cret.ref[1], r);
				emit(Oadd, Kl, r, i1->to, getcon(8, fn));
			}
			cret.ref[0] = newtmp("abi", cret.cls[0], fn);
			emit(Ostorel, 0, R, cret.ref[0], i1->to);
			ca += retr(reg, &cret);
			if (cret.size > 8)
				emit(Ocopy, cret.cls[1], cret.ref[1], reg[1], R);
			emit(Ocopy, cret.cls[0], cret.ref[0], reg[0], R);
		} else
			cty |= 1 << 13;
		/* allocate return pad */
		il = alloc(sizeof *il);
		/* specific to NAlign == 3 */
		al = cret.align >= 2 ? cret.align - 2 : 0;
		il->i = (Ins){Oalloc+al, i1->to, {getcon(cret.size, fn)}, Kl};
		il->link = *ilp;
		*ilp = il;
	} else {
		il = 0;
		if (KBASE(i1->cls) == 0) {
			emit(Ocopy, i1->cls, i1->to, TMP(R0), R);
			cty |= 1;
		} else {
			emit(Ocopy, i1->cls, i1->to, TMP(V0), R);
			cty |= 1 << 2;
		}
	}
	envc = !req(R, env);
	varc = i1->op == Ovacall;
	if (varc && envc)
		err("sysv abi does not support variadic env calls");
	ca |= (varc | envc) << 12;
	emit(Ocall, i1->cls, R, i1->arg[0], CALL(ca));
	if (envc)
		emit(Ocopy, Kl, TMP(RAX), env, R);
	if (varc)
		emit(Ocopy, Kw, TMP(RAX), getcon((ca >> 8) & 15, fn), R);

	ng = nf = 0;
	if (il && cret.inmem)
		emit(Ocopy, Kl, rarg(Kl, &ng, &nf), il->i.to, R); /* pass hidden argument */
	for (i=i0, a=ac; i<i1; i++, a++) {
		if (a->inmem)
			continue;
		r1 = rarg(a->cls[0], &ng, &nf);
		if (i->op == Oargc) {
			if (a->size > 8) {
				r2 = rarg(a->cls[1], &ng, &nf);
				r = newtmp("abi", Kl, fn);
				emit(Oload, a->cls[1], r2, r, R);
				emit(Oadd, Kl, r, i->arg[1], getcon(8, fn));
			}
			emit(Oload, a->cls[0], r1, i->arg[1], R);
		} else
			emit(Ocopy, i->cls, r1, i->arg[0], R);
	}

	if (!stk)
		return;

	r = newtmp("abi", Kl, fn);
	for (i=i0, a=ac, off=0; i<i1; i++, a++) {
		if (!a->inmem)
			continue;
		if (i->op == Oargc) {
			if (a->align == 4)
				off += off & 15;
			blit(r, off, i->arg[1], a->size, fn);
		} else {
			r1 = newtmp("abi", Kl, fn);
			emit(Ostorel, 0, R, i->arg[0], r1);
			emit(Oadd, Kl, r1, r, getcon(off, fn));
		}
		off += a->size;
	}
	emit(Osalloc, Kl, r, getcon(stk, fn), R);
}

#if 0

static int
selpar(Fn *fn, Ins *i0, Ins *i1)
{
	AClass *ac, *a, aret;
	Ins *i;
	int ng, nf, s, al, fa;
	Ref r, env;

	env = R;
	ac = alloc((i1-i0) * sizeof ac[0]);
	curi = &insb[NIns];
	ng = nf = 0;

	if (fn->retty >= 0) {
		typclass(&aret, &typ[fn->retty]);
		fa = argsclass(i0, i1, ac, Opar, &aret, &env);
	} else
		fa = argsclass(i0, i1, ac, Opar, 0, &env);

	for (i=i0, a=ac; i<i1; i++, a++) {
		if (i->op != Oparc || a->inmem)
			continue;
		if (a->size > 8) {
			r = newtmp("abi", Kl, fn);
			a->ref[1] = newtmp("abi", Kl, fn);
			emit(Ostorel, 0, R, a->ref[1], r);
			emit(Oadd, Kl, r, i->to, getcon(8, fn));
		}
		a->ref[0] = newtmp("abi", Kl, fn);
		emit(Ostorel, 0, R, a->ref[0], i->to);
		/* specific to NAlign == 3 */
		al = a->align >= 2 ? a->align - 2 : 0;
		emit(Oalloc+al, Kl, i->to, getcon(a->size, fn), R);
	}

	if (fn->retty >= 0 && aret.inmem) {
		r = newtmp("abi", Kl, fn);
		emit(Ocopy, Kl, r, rarg(Kl, &ng, &nf), R);
		fn->retr = r;
	}

	for (i=i0, a=ac, s=4; i<i1; i++, a++) {
		switch (a->inmem) {
		case 1:
			if (a->align > 4)
				err("sysv abi requires alignments of 16 or less");
			if (a->align == 4)
				s = (s+3) & -4;
			fn->tmp[i->to.val].slot = -s;
			s += a->size / 4;
			continue;
		case 2:
			emit(Oload, i->cls, i->to, SLOT(-s), R);
			s += 2;
			continue;
		}
		r = rarg(a->cls[0], &ng, &nf);
		if (i->op == Oparc) {
			emit(Ocopy, Kl, a->ref[0], r, R);
			if (a->size > 8) {
				r = rarg(a->cls[1], &ng, &nf);
				emit(Ocopy, Kl, a->ref[1], r, R);
			}
		} else
			emit(Ocopy, i->cls, i->to, r, R);
	}

	if (!req(R, env))
		emit(Ocopy, Kl, env, TMP(RAX), R);

	return fa | (s*4)<<12;
}

void
xv_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0, *ip;
	RAlloc *ral;
	int n, fa;

	for (b=fn->start; b; b=b->link)
		b->visit = 0;

	/* lower parameters */
	for (b=fn->start, i=b->ins; i-b->ins<b->nins; i++)
		if (!ispar(i->op))
			break;
	fa = selpar(fn, b->ins, i);
	n = b->nins - (i - b->ins) + (&insb[NIns] - curi);
	i0 = alloc(n * sizeof(Ins));
	ip = icpy(ip = i0, curi, &insb[NIns] - curi);
	ip = icpy(ip, i, &b->ins[b->nins] - i);
	b->nins = n;
	b->ins = i0;

	/* lower calls, returns, and vararg instructions */
	ral = 0;
	b = fn->start;
	do {
		if (!(b = b->link))
			b = fn->start; /* do it last */
		if (b->visit)
			continue;
		curi = &insb[NIns];
		selret(b, fn);
		for (i=&b->ins[b->nins]; i!=b->ins;)
			switch ((--i)->op) {
			default:
				emiti(*i);
				break;
			case Ocall:
			case Ovacall:
				for (i0=i; i0>b->ins; i0--)
					if (!isarg((i0-1)->op))
						break;
				selcall(fn, i0, i, &ral);
				i = i0;
				break;
			case Ovastart:
			case Ovaarg:
				die("vastart and vaarg are todo for arm");
				break;
			case Oarg:
			case Oargc:
				die("unreachable");
			}
		if (b == fn->start)
			for (; ral; ral=ral->link)
				emiti(ral->i);
		b->nins = &insb[NIns] - curi;
		idup(&b->ins, curi, b->nins);
	} while (b != fn->start);

	if (debug['A']) {
		fprintf(stderr, "\n> After ABI lowering:\n");
		printfn(fn, stderr);
	}
}

#endif
