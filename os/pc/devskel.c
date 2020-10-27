#include	"dat.h"
#include	"fns.h"
#include	"error.h"

enum {
	Qdir,
	Qdata,
	Qctl,
};

Dirtab skeltab[] =
{
	".",		{Qdir, 0, QTDIR},	0,	0555,
	"skeldata",	{Qdata},	0,	0666,
	"skelctl",	{Qctl},	0,	0666,
};

static Chan *
skelattach(char *spec)
{
	return devattach('S', spec);
}

static Walkqid *
skelwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, skeltab, nelem(skeltab), devgen);
}

static int
skelstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, skeltab, nelem(skeltab), devgen);
}

static Chan *
skelopen(Chan *c, int omode)
{
	return devopen(c, omode, skeltab, nelem(skeltab), devgen);
}

static void
skelclose(Chan *c)
{
	USED(c);
}

static long
skelread(Chan *c, void *va, long count, vlong offset)
{
	int n;
	//USED(offset);
	
	if(c->qid.type & QTDIR)
		return devdirread(c, va, count, skeltab, nelem(skeltab), devgen);
	
	switch (c->qid.path) {
		case Qdata:
			n = readstr(offset, va, count, "Read from data file\n");
			break;
		case Qctl:
			n = readstr(offset, va, count, "Read from control file\n");
			break;
	}

	return n;
}

static long
skelwrite(Chan *c, void *va, long count, vlong offset)
{
	USED(c);
	USED(va);
	USED(offset);

	return count;
}

Dev skeldevtab = {
	'S',
	"skel",

	devinit,
	skelattach,
	skelwalk,
	skelstat,
	skelopen,
	devcreate,
	skelclose,
	skelread,
	devbread,
	skelwrite,
	devbwrite,
	devremove,
	devwstat,
};
