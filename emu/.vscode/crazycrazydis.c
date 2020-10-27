#include	"dat.h"
#include	"fns.h"
#include	<isa.h>
#include	<interp.h>
#include	<kernel.h>
#include	"error.h"
#include	"raise.h"

struct
{
	Lock	l;
	Prog*	runhd;
	Prog*	runtl;
	Prog*	head;
	Prog*	tail;
	Rendez	irend;
	int	idle;
	int	nyield;
	int	creating;
	Proc*	vmq;		/* queue of procs wanting vm */
	Proc*	vmqt;
	Proc*	idlevmq;	/* queue of procs wanting work */
	Atidle*	idletasks;
} isched;

int	bflag;
int	cflag;
uvlong	gcbusy;
uvlong	gcidle;
uvlong	gcidlepass;
uvlong	gcpartial;
int keepbroken = 1;
extern int	vflag;
static Prog*	proghash[64];

static Progs*	delgrp(Prog*);
static void	addgrp(Prog*, Prog*);
void	printgrp(Prog*, char*);
void printQueuePrior(Prog *qhead, Prog *qrunhead, Prog *);
extern ulong kerndate;
extern char* eve;

//THIS PART IS FOR GETTING THE TIME
// -----------------------------------------------------------------------
int dmsize[12] = { 31, 28, 31, 30, 31, 30,
                      31, 31, 30, 31, 30, 31};

int ldmsize[12] = { 31, 29, 31, 30, 31, 30,
                    31, 31, 30, 31, 30, 31};

char* wkday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
char * month[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

typedef struct Tim {
    int sec; //seconds (0 to 59)
    int min; //minutes (0 to 59)
    int hour; //hours (0 to 23)
    int mday; // day of the month
    int mon; //month (0 to 11)
    int year; // year-1900; 2000AD is 100
    int wday; //day of week (0 to 6, Sunday is 0)
    int yday;// day of year (0 to 365)
    char * zone; // time zone name
    int tzoff; // time zone offset (seconds from GMT)
} Tim;

Tim getTime(int tim) {
    Tim xtime;

    //break initial number into days
    int hms = tim%86400;
    int day = tim/86400;
    if (hms < 0) {
        hms == 86400;
        day -= 1;
    }

    //generate hour;minte;seconds
    xtime.sec = hms % 60;
    int d1 = hms/60;
    xtime.min = d1 % 60;
    d1 /= 60;
    xtime.hour = d1;

    //day is the day number
    xtime.wday = (day + 7340036) % 7 ;

    //yera number
    if (day >= 0) {
        for (d1=70; day >=dysize(d1+ 1900); d1++) {
            day -= dysize(d1 + 1900);
        }
    } else {
        for (d1=70; day < 0; d1--) {
            day += dysize(d1+1900-1);
        }
    }
    xtime.year = d1;
    int d0 = day;
    xtime.yday = d0;

    //generate month
    int* dmsz;
    if (dysize(d1 + 1900) == 366) {
        dmsz = ldmsize;
    } else {
        dmsz = dmsize;
    }
    for (d1=0; d0>=dmsz[d1]; d1++) {
        d0 -= dmsz[d1];
    }
    xtime.mday = d0 +1;
    xtime.mon = d1;
    xtime.zone = "EST";
    xtime.tzoff = -4;
    return xtime;
}

//-------------------------------------------------------------------



static Prog**
pidlook(int pid)
{
	ulong h;
	Prog **l;

	h = (ulong)pid % nelem(proghash);
	for(l = &proghash[h]; *l != nil && (*l)->pid != pid; l = &(*l)->pidlink)
		;
	return l;
}

int
tready(void *a)
{
	USED(a);
	return isched.runhd != nil || isched.vmq != nil;
}

Prog*
progpid(int pid)
{
	return *pidlook(pid);
}

Prog*
progn(int n)
{
	Prog *p;

	for(p = isched.head; p && n--; p = p->next)
		;
	return p;
}

int
nprog(void)
{
	int n;
	Prog *p;

	n = 0;
	for(p = isched.head; p; p = p->next)
		n++;
	return n;
}

static void
execatidle(void)
{
	int done;

	if(tready(nil))
		return;

	gcidle++;
	up->type = IdleGC;
	up->iprog = nil;
	addrun(up->prog);
	done = gccolor+3;
	while(gccolor < done && gcruns()) {
		if(isched.vmq != nil || isched.runhd != isched.runtl) {
			gcpartial++;
			break;
		}
		rungc(isched.head);
		gcidlepass++;
		if(((ulong)gcidlepass&0xFF) == 0)
			osyield();
	}
	up->type = Interp;
	delrunq(up->prog);
}

Prog*
newprog(Prog *p, Modlink *m)
{
	Heap *h;
	Prog *n, **ph;
	Osenv *on, *op;
	static int pidnum;

	if(p != nil){
		if(p->group != nil)
			p->flags |= p->group->flags & Pkilled;
		if(p->kill != nil)
			error(p->kill);
		if(p->flags & Pkilled)
			error("");
	}
	n = malloc(sizeof(Prog)+sizeof(Osenv));
	if(n == 0){
		if(p == nil)
			panic("no memory");
		else
			error(exNomem);
	}

	n->pid = ++pidnum;
	if(n->pid <= 0)
		panic("no pids");
	n->group = nil;

	if(isched.tail != nil) {
		n->prev = isched.tail;
		isched.tail->next = n;
	}
	else {
		isched.head = n;
		n->prev = nil;
	}
	isched.tail = n;

	ph = pidlook(n->pid);
	if(*ph != nil)
		panic("dup pid");
	n->pidlink = nil;
	*ph = n;

	n->osenv = (Osenv*)((uchar*)n + sizeof(Prog));
	n->xec = xec;
	n->quanta = PQUANTA;
	n->flags = 0;
	n->exval = H;
	n->nice = 0;
	n->priority = 0;

	h = D2H(m);
	h->ref++;
	Setmark(h);
	n->R.M = m;
	n->R.MP = m->MP;
	if(m->MP != H)
		Setmark(D2H(m->MP));
	addrun(n);

	if(p == nil){
		newgrp(n);
		return n;
	}

	addgrp(n, p);
	n->flags = p->flags;
	if(p->flags & Prestrict)
		n->flags |= Prestricted;
	memmove(n->osenv, p->osenv, sizeof(Osenv));
	op = p->osenv;
	on = n->osenv;
	on->waitq = op->childq;
	on->childq = nil;
	on->debug = nil;
	incref(&on->pgrp->r);
	incref(&on->fgrp->r);
	incref(&on->egrp->r);
	if(on->sigs != nil)
		incref(&on->sigs->r);
	on->user = nil;
	kstrdup(&on->user, op->user);
	on->errstr = on->errbuf0;
	on->syserrstr = on->errbuf1;

	return n;
}

void
delprog(Prog *p, char *msg)
{
	Osenv *o;
	Prog **ph;

	tellsomeone(p, msg);	/* call before being removed from prog list */

	o = p->osenv;
	release();
	closepgrp(o->pgrp);
	closefgrp(o->fgrp);
	closeegrp(o->egrp);
	closesigs(o->sigs);
	acquire();

	delgrp(p);

	if(p->prev)
		p->prev->next = p->next;
	else
		isched.head = p->next;

	if(p->next)
		p->next->prev = p->prev;
	else
		isched.tail = p->prev;

	ph = pidlook(p->pid);
	if(*ph == nil)
		panic("lost pid");
	*ph = p->pidlink;

	if(p == isched.runhd) {
		isched.runhd = p->link;
		if(p->link == nil)
			isched.runtl = nil;
	}
	p->state = 0xdeadbeef;
	free(o->user);
	free(p->killstr);
	free(p->exstr);
	free(p);
}

void
renameproguser(char *old, char *new)
{
	Prog *p;
	Osenv *o;

	acquire();
	for(p = isched.head; p; p = p->next){
		o = p->osenv;
		if(o->user != nil && strcmp(o->user, old) == 0)
			kstrdup(&o->user, new);
	}
	release();
}

void
tellsomeone(Prog *p, char *buf)
{
	Osenv *o;

	if(waserror())
		return;
	o = p->osenv;
	if(o->childq != nil)
		qproduce(o->childq, buf, strlen(buf));
	if(o->waitq != nil) 
		qproduce(o->waitq, buf, strlen(buf)); 
	poperror();
}

static void
swiprog(Prog *p)
{
	Proc *q;

	lock(&procs.l);
	for(q = procs.head; q; q = q->next) {
		if(q->iprog == p) {
			unlock(&procs.l);
			swiproc(q, 1);
			return;
		}
	}
	unlock(&procs.l);
	/*print("didn't find\n");*/
}

static Prog*
grpleader(Prog *p)
{
	Progs *g;
	Prog *l;

	g = p->group;
	if(g != nil && (l = g->head) != nil && l->pid == g->id)
		return l;
	return nil;
}

int
exprog(Prog *p, char *exc)
{
	/* similar code to killprog but not quite */
	switch(p->state) {
	case Palt:
		altdone(p->R.s, p, nil, -1);
		break;
	case Psend:
		cqdelp(&p->chan->send, p);
		break;
	case Precv:
		cqdelp(&p->chan->recv, p);
		break;
	case Pready:
		break;
	case Prelease:
		swiprog(p);
		break;
	case Pexiting:
	case Pbroken:
	case Pdebug:
		return 0;
	default:
		panic("exprog - bad state 0x%x\n", p->state);
	}
	if(p->state != Pready && p->state != Prelease)
		addrun(p);
	if(p->kill == nil){
		if(p->killstr == nil){
			p->killstr = malloc(ERRMAX);
			if(p->killstr == nil){
				p->kill = Enomem;
				return 1;
			}
		}
		kstrcpy(p->killstr, exc, ERRMAX);
		p->kill = p->killstr;
	}
	return 1;
}

static void
propex(Prog *p, char *estr)
{
	Prog *f, *nf, *pgl;

	if(!(p->flags & (Ppropagate|Pnotifyleader)) || p->group == nil)
		return;
	if(*estr == 0){
		if((p->flags & Pkilled) == 0)
			return;
		estr = "killed";
	}
	pgl = grpleader(p);
	if(pgl == nil)
		pgl = p;
	if(!(pgl->flags & (Ppropagate|Pnotifyleader)))
		return;	/* exceptions are local; don't propagate */
	for(f = p->group->head; f != nil; f = nf){
		nf = f->grpnext;
		if(f != p && f != pgl){
			if(pgl->flags & Ppropagate)
				exprog(f, estr);
			else{
				f->flags &= ~(Ppropagate|Pnotifyleader);	/* prevent recursion */
				killprog(f, "killed");
			}
		}
	}
	if(p != pgl)
		exprog(pgl, estr);
}

int
killprog(Prog *p, char *cause)
{
	Osenv *env;
	char msg[ERRMAX+2*KNAMELEN];

	if(p == isched.runhd) {
		p->kill = "";
		p->flags |= Pkilled;
		p->state = Pexiting;
		return 0;
	}

	switch(p->state) {
	case Palt:
		altdone(p->R.s, p, nil, -1);
		break;
	case Psend:
		cqdelp(&p->chan->send, p);
		break;
	case Precv:
		cqdelp(&p->chan->recv, p);
		break;
	case Pready:
		delrunq(p);
		break;
	case Prelease:
		p->kill = "";
		p->flags |= Pkilled;
		p->state = Pexiting;
		swiprog(p);
		/* No break */
	case Pexiting:
		return 0;
	case Pbroken:
	case Pdebug:
		break;
	default:
		panic("killprog - bad state 0x%x\n", p->state);
	}

	if(p->addrun != nil) {
		p->kill = "";
		p->flags |= Pkilled;
		p->addrun(p);
		p->addrun = nil;
		return 0;
	}

	env = p->osenv;
	if(env->debug != nil) {
		p->state = Pbroken;
		dbgexit(p, 0, cause);
		return 0;
	}

	propex(p, "killed");

	snprint(msg, sizeof(msg), "%d \"%s\":%s", p->pid, p->R.M->m->name, cause);

	p->state = Pexiting;
	gclock();
	destroystack(&p->R);
	delprog(p, msg);
	gcunlock();

	return 1;
}

void
newgrp(Prog *p)
{
	Progs *pg, *g;

	if(p->group != nil && p->group->id == p->pid)
		return;
	g = malloc(sizeof(*g));
	if(g == nil)
		error(Enomem);
	p->flags &= ~(Ppropagate|Pnotifyleader);
	g->id = p->pid;
	g->flags = 0;
	if(p->group != nil)
		g->flags |= p->group->flags&Pprivatemem;
	g->child = nil;
	pg = delgrp(p);
	g->head = g->tail = p;
	p->group = g;
	if(pg != nil){
		g->sib = pg->child;
		pg->child = g;
	}
	g->parent = pg;
}

static void
addgrp(Prog *n, Prog *p)
{
	Progs *g;

	n->group = p->group;
	if((g = n->group) != nil){
		n->grpnext = nil;
		if(g->head != nil){
			n->grpprev = g->tail;
			g->tail->grpnext = n;
		}else{
			n->grpprev = nil;
			g->head = n;
		}
		g->tail = n;
	}
}

static Progs*
delgrp(Prog *p)
{
	Progs *g, *pg, *cg, **l;

	g = p->group;
	if(g == nil)
		return nil;
	if(p->grpprev)
		p->grpprev->grpnext = p->grpnext;
	else
		g->head = p->grpnext;
	if(p->grpnext)
		p->grpnext->grpprev = p->grpprev;
	else
		g->tail = p->grpprev;
	p->grpprev = p->grpnext = nil;
	p->group = nil;

	if(g->head == nil){
		/* move up, giving subgroups of groups with no Progs to their parents */
		do{
			if((pg = g->parent) != nil){
				pg = g->parent;
				for(l = &pg->child; *l != nil && *l != g; l = &(*l)->sib)
					;
				*l = g->sib;
			}
			/* put subgroups in new parent group */
			while((cg = g->child) != nil){
				g->child = cg->sib;
				cg->parent = pg;
				if(pg != nil){
					cg->sib = pg->child;
					pg->child = cg;
				}
			}
			free(g);
		}while((g = pg) != nil && g->head == nil);
	}
	return g;
}

void
printgrp(Prog *p, char *v)
{
	Progs *g;
	Prog *q;

	g = p->group;
	print("%s pid %d grp %d pgrp %d: [pid", v, p->pid, g->id, g->parent!=nil?g->parent->id:0);
	for(q = g->head; q != nil; q = q->grpnext)
		print(" %d", q->pid);
	print(" subgrp");
	for(g = g->child; g != nil; g = g->sib)
		print(" %d", g->id);
	print("]\n");
}

int
killgrp(Prog *p, char *msg)
{
	int i, npid, *pids;
	Prog *f;
	Progs *g;

	/* interpreter has been acquired */
	g = p->group;
	if(g == nil || g->head == nil)
		return 0;
	while(g->flags & Pkilled){
		release();
		acquire();
	}
	npid = 0;
	for(f = g->head; f != nil; f = f->grpnext)
		if(f->group != g)
			panic("killgrp");
		else
			npid++;
	/* use pids not Prog* because state can change during killprog (eg, in delprog) */
	pids = malloc(npid*sizeof(int));
	if(pids == nil)
		error(Enomem);
	npid = 0;
	for(f = g->head; f != nil; f = f->grpnext)
		pids[npid++] = f->pid;
	g->flags |= Pkilled;
	if(waserror()) {
		g->flags &= ~Pkilled;
		free(pids);
		nexterror();
	}
	for(i = 0; i < npid; i++) {
		f = progpid(pids[i]);
		if(f != nil && f != currun())
			killprog(f, msg);
	}
	poperror();
	g->flags &= ~Pkilled;
	free(pids);
	return 1;
}

char	changup[] = "channel hangup";

void
killcomm(Progq **q)
{
	Prog *p;
	Progq *f;

	for (f = *q; f != nil; f = *q) {
		*q = f->next;
		p = f->prog;
		free(f);
		if(p == nil)
			return;
		p->ptr = nil;
		switch(p->state) {
		case Prelease:
			swiprog(p);
			break;
		case Psend:
		case Precv:
			p->kill = changup;
			addrun(p);
			break;
		case Palt:
			altgone(p);
			break;
		}
	}
}

void
addprog(Proc *p)
{
	Prog *n;

	n = malloc(sizeof(Prog));
	if(n == nil)
		panic("no memory");
	p->prog = n;
	n->osenv = p->env;
}

static void
cwakeme(Prog *p)
{
	Osenv *o;

	p->addrun = nil;
	o = p->osenv;
	Wakeup(o->rend);
}

static int
cdone(void *vp)
{
	Prog *p = vp;

	return p->addrun == nil || p->kill != nil;
}

void
cblock(Prog *p)
{
	Osenv *o;

	p->addrun = cwakeme;
	o = p->osenv;
	o->rend = &up->sleep;
	release();

	/*
	 * To allow cdone(p) safely after release,
	 * p must be currun before the release.
	 * Exits in the error case with the vm acquired.
	 */
	if(waserror()) {
		acquire();
		p->addrun = nil;
		nexterror();
	}
	Sleep(o->rend, cdone, p);
	if (p->kill != nil)
		error(Eintr);
	poperror();
	acquire();
}


void sortedInsert(struct Prog** head_ref, struct Prog* new_node) 
{ 
    struct Prog* current; 
    /* Special case for the head end */
    if (*head_ref == nil) 
    { 
        new_node->link = *head_ref; 
        *head_ref = new_node; 
		//print("%d(%d) is only process in ready queue\n", new_node->pid, new_node->priority);
    } else if  ((*head_ref)->priority >= new_node->priority) {
		//new_node->link = (*head_ref)->link;
		//(*head_ref)->link = new_node;
		new_node->link = isched.runhd->link;
		isched.runhd->link = new_node;
		//print("%d(%d)is added in 2nd position\n", new_node->pid, new_node->priority);
	} else { 
        /* Locate the node before the point of insertion */
        current = *head_ref; 
        while (current->link!=nil && 
               current->link->priority < new_node->priority) 
        { 
            current = current->link; 
        } 
        new_node->link = current->link; 	
		current->link = new_node; 

		if (new_node->link == nil) {
		isched.runtl = new_node;
		//print("Added %d(%d) right before nil\n", new_node->pid, new_node->priority);
		} else {
			//print("Added %d(%d) right before %d(%d)\n", new_node->pid, new_node->priority, new_node->link->pid, new_node->link->priority);
		}
	} 

} 

void
addrun(Prog *p)
{
	if(p->addrun != 0) {
		p->addrun(p);
		return;
	}

	int c = p->ticks/340;
	if(c > 255) {
		c = 255;
	}
	int temp_priority = c/16 + 100 + p->nice;
	
	if (temp_priority >= 127) {
		p->priority = 127;
	} else {
		p->priority = temp_priority;
	}

	p->state = Pready;
	p->link = nil;
	sortedInsert(&isched.runhd, p);
	printQueuePrior(isched.head, isched.runhd, isched.tail);

	// if(isched.runhd == nil)
	// 	isched.runhd = p;
	// else
	// 	isched.runtl->link = p;

	// isched.runtl = p;
}

Prog*
delrun(int state)
{
	Prog *p;

	p = isched.runhd;
	p->state = state;
	isched.runhd = p->link;
	if(p->link == nil)
		isched.runtl = nil;

	return p;
}

void
delrunq(Prog *p)
{
	Prog *prev, *f;

	prev = nil;
	for(f = isched.runhd; f; f = f->link) {
		if(f == p)
			break;
		prev = f;
	}
	if(f == nil)
		return;
	if(prev == nil)
		isched.runhd = p->link;
	else
		prev->link = p->link;
	if(p == isched.runtl)
		isched.runtl = prev;
}

Prog*
delruntail(int state)
{
	Prog *p;

	p = isched.runtl;
	delrunq(p);
	p->state = state;
	return p;
}

Prog*
currun(void)
{
	return isched.runhd;
}

Prog*
schedmod(Module *m)
{
	Heap *h;
	Type *t;
	Prog *p;
	Modlink *ml;
	Frame f, *fp;

	ml = mklinkmod(m, 0);

	if(m->origmp != H && m->ntype > 0) {
		t = m->type[0];
		h = nheap(t->size);
		h->t = t;
		t->ref++;
		ml->MP = H2D(uchar*, h);
		newmp(ml->MP, m->origmp, t);
	}

	p = newprog(nil, ml);
	h = D2H(ml);
	h->ref--;
	p->R.PC = m->entry;
	fp = &f;
	R.s = &fp;
	f.t = m->entryt;
	newstack(p);
	initmem(m->entryt, p->R.FP);

	return p;
}

/*
static char*
m(Prog *p)
{
	if(p)
		if(p->R.M)
			if(p->R.M->m)
				return p->R.M->m->name;
	return "nil";
}
*/

void
acquire(void)
{
	int empty;
	Prog *p;

	lock(&isched.l);
	if(isched.idle) {
		isched.idle = 0;
		unlock(&isched.l);
	}
	else {
		up->qnext = nil;
		if(isched.vmq != nil){
			empty = 0;
			isched.vmqt->qnext = up;
		}else{
			isched.vmq = up;
			empty = 1;
		}
		isched.vmqt = up;

		unlock(&isched.l);
		strcpy(up->text, "acquire");
		if(empty)
			Wakeup(&isched.irend);
		osblock();
	}

	if(up->type == Interp) {
		p = up->iprog;
		up->iprog = nil;
		irestore(p);
	}
	else
		p = up->prog;

	p->state = Pready;
	p->link = isched.runhd;
	isched.runhd = p;
	if(p->link == nil)
		isched.runtl = p;

	strcpy(up->text, "dis");
}

void
release(void)
{
	Proc *p, **pq;
	int f;

	if(up->type == Interp)
		up->iprog = isave();
	else
		delrun(Prelease);

	lock(&isched.l);
	if(*(pq = &isched.vmq) == nil && *(pq = &isched.idlevmq) == nil) {
		isched.idle = 1;
		f = isched.creating;
		isched.creating = 1;
		unlock(&isched.l);
		if(f == 0)
			kproc("dis", vmachine, nil, 0);
		return;
	}
	p = *pq;
	*pq = p->qnext;
	unlock(&isched.l);

	osready(p);		/* wake up thread to run VM */
	strcpy(up->text, "released");
}

void
iyield(void)
{
	Proc *p;

	lock(&isched.l);
	p = isched.vmq;
	if(p == nil) {
		unlock(&isched.l);
		return;
	}
	isched.nyield++;
	isched.vmq = p->qnext;

	if(up->iprog != nil)
		panic("iyield but iprog, type %d", up->type);
	if(up->type != Interp){
		static int once;
		if(!once++)
			print("tell charles: #%p->type==%d\n", up, up->type);
	}
	up->qnext = isched.idlevmq;
	isched.idlevmq = up;

	unlock(&isched.l);
	osready(p);		/* wake up acquiring kproc */
	strcpy(up->text, "yield");
	osblock();		/* sleep */
	strcpy(up->text, "dis");
}

void
startup(void)
{

	up->type = Interp;
	up->iprog = nil;

	lock(&isched.l);
	isched.creating = 0;
	if(isched.idle) {
		isched.idle = 0;
		unlock(&isched.l);
		return;
	}
	up->qnext = isched.idlevmq;
	isched.idlevmq = up;
	unlock(&isched.l);

	osblock();
}

void
progexit(void)
{
	Prog *r;
	Module *m;
	int broken;
	char *estr, msg[ERRMAX+2*KNAMELEN];

	estr = up->env->errstr;
	broken = 0;
	if(estr[0] != '\0' && strcmp(estr, Eintr) != 0 && strncmp(estr, "fail:", 5) != 0)
		broken = 1;

	r = up->iprog;
	if(r != nil)
		acquire();
	else
		r = currun();

	if(*estr == '\0' && r->flags & Pkilled)
		estr = "killed";

	m = R.M->m;
	if(broken){
		if(cflag){	/* only works on Plan9 for now */
			char *pc = strstr(estr, "pc=");

			if(pc != nil)
				R.PC = r->R.PC = (Inst*)strtol(pc+3, nil, 0);	/* for debugging */
		}
		print("[%s] Broken: \"%s\"\n", m->name, estr);
	}

	snprint(msg, sizeof(msg), "%d \"%s\":%s", r->pid, m->name, estr);

	if(up->env->debug != nil) {
		dbgexit(r, broken, estr);
		broken = 1;
		/* must force it to break if in debug */
	}else if(broken && (!keepbroken || strncmp(estr, "out of memory", 13)==0 || memusehigh()))
		broken = 0;	/* don't want them or short of memory */

	if(broken){
		tellsomeone(r, msg);
		r = isave();
		r->state = Pbroken;
		return;
	}

	gclock();
	destroystack(&R);
	delprog(r, msg);
	gcunlock();

	if(isched.head == nil)
		cleanexit(0);
}

void
disfault(void *reg, char *msg)
{
	Prog *p;

	USED(reg);

	if(strncmp(msg, Eintr, 6) == 0)
		exits(0);

	if(up == nil) {
		print("EMU: faults: %s\n", msg);
		cleanexit(0);
	}
	if(up->type != Interp) {
		print("SYS: process %s faults: %s\n", up->text, msg);
		cleanexit(0);
	}

	if(up->iprog != nil)
		acquire();

	p = currun();
	if(p == nil)
		panic("Interp faults with no dis prog");

	/* cause an exception in the dis prog.  As for error(), but Plan 9 needs reg*/
	kstrcpy(up->env->errstr, msg, ERRMAX);
	oslongjmp(reg, up->estack[--up->nerr], 1);
}

inline void printQueuePrior(Prog *qhead, Prog * qrunhead, Prog * qtail) {
	Prog *x = qrunhead;
	// print("All: ");
	// while(qhead != nil) {
	// 	print( "%d(%d)->", qhead->pid, qhead->priority);
	// 	qhead = qhead->next;
	// }
	print("\nRun: ");
	while(x != nil) {
		print( "%d(%d, %d)->", x->pid, x->priority, x->nice);
		x = x->link;
	}
	// print("\nReverse: ");
	// while(qtail != nil) {
	// 	print("%d(%d, %d)->", qtail->pid, qtail->priority, qtail->nice);
	// 	qtail = qtail->prev;
	// }
	print("\n\n");
}


void
vmachine(void *a)
{
	Prog *r;
	Osenv *o;
	int cycles, count;
	static int gccounter;

	USED(a);

	startup();

	while(waserror()) {
		if(up->iprog != nil)
			acquire();
		if(handler(up->env->errstr) == 0) {
			propex(currun(), up->env->errstr);
			progexit();
		}
		up->env = &up->defenv;
	}

	cycles = 0;
	for(;;) {
		count++;
		if(tready(nil) == 0) {
			execatidle();
			strcpy(up->text, "idle");
			Sleep(&isched.irend, tready, 0);
			strcpy(up->text, "dis");
		}

		if(isched.vmq != nil && (isched.runhd == nil || ++cycles > 2)){
			iyield();
			cycles = 0;
		}
		
		r = isched.runhd;
		if(r != nil) {
			o = r->osenv;
			up->env = o;
			FPrestore(&o->fpu);
			r->xec(r);
			FPsave(&o->fpu);

			if (count % 20) {
				int c = r->ticks/340;
				if(c > 255) {
					c = 255;
				}
				int temp_priority = c/16 + 100 + r->nice;
				
				if (temp_priority >= 127) {
					r->priority = 127;
				} else {
					r->priority = temp_priority;
				}
			}


			if(isched.runhd != nil) {
				if(r == isched.runhd) {
					r->priority++;
					if(r->priority >127) { r->priority = 127; }
					if(isched.runhd != isched.runtl) {
						isched.runhd = r->link;
						sortedInsert(&isched.runhd, r);
					}
				}
			}

			up->env = &up->defenv;
		}
		if(isched.runhd != nil)
		if((++gccounter&0xFF) == 0 || memlow()) {
			gcbusy++;
			up->type = BusyGC;
			pushrun(up->prog);
			rungc(isched.head);
			up->type = Interp;
			delrunq(up->prog);
		}
	}
}

void
disinit(void *a)
{
	Prog *p;
	Osenv *o;
	Module *root;
	char *initmod = a;

	if(waserror())
		panic("disinit error: %r");

	if(vflag)
		print("Initial Dis: \"%s\"\n", initmod);

	fmtinstall('D', Dconv);
	
	print("Host system owner: %s\n", eve);
	Tim time_struct = getTime(kerndate);
	int year = time_struct.year + 1900;
	print("%s %s %.2d %.2d:%.2d:%.2d %s %d\n", 
            wkday[time_struct.wday],
            month[time_struct.mon],
            time_struct.mday,
            time_struct.hour + time_struct.tzoff,
            time_struct.min,
            time_struct.sec,
            time_struct.zone,
            year);
	


	FPinit();
	FPsave(&up->env->fpu);

	opinit();
	modinit();
	excinit();

	root = load(initmod);
	if(root == 0) {
		kgerrstr(up->genbuf, sizeof up->genbuf);
		panic("loading \"%s\": %s", initmod, up->genbuf);
	}

	p = schedmod(root);

	memmove(p->osenv, up->env, sizeof(Osenv));
	o = p->osenv;
	incref(&o->pgrp->r);
	incref(&o->fgrp->r);
	incref(&o->egrp->r);
	if(o->sigs != nil)
		incref(&o->sigs->r);
	o->user = nil;
	kstrdup(&o->user, up->env->user);
	o->errstr = o->errbuf0;
	o->syserrstr = o->errbuf1;

	isched.idle = 1;
	poperror();
	vmachine(nil);
}

void
pushrun(Prog *p)
{
	if(p->addrun != nil)
		panic("pushrun addrun");
	p->state = Pready;
	p->link = isched.runhd;
	isched.runhd = p;
	if(p->link == nil)
		isched.runtl = p;
}
