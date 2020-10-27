#include	"dat.h"
#include	"fns.h"
#include	"error.h"

#define BLOCKSIZE 1024

static Chan * chan0;
static Chan * chan1;

enum {
	Qdir,
    Qdata,
    Qctl,
};

Dirtab raid0tab[] =
{
	".",		{Qdir, 0, QTDIR},	0,	0555,
	"raid0data",	{Qdata},	0,	0666,
	"raid0ctl",	{Qctl},	0,	0666,
};

enum {
    CMechohello,
    CMbind,
};

static 
Cmdtab raid0cmd [] = {
    CMechohello, "sayhello", 1,
    CMbind, "bind", 3,
};



static Chan *
raid0attach(char *spec)
{

	return devattach('R', spec);
}

static Walkqid *
raid0walk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, raid0tab, nelem(raid0tab), devgen);
}

static int
raid0stat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, raid0tab, nelem(raid0tab), devgen);
}

static Chan *
raid0open(Chan *c, int omode)
{
	return devopen(c, omode, raid0tab, nelem(raid0tab), devgen);
}

static void
raid0close(Chan *c)
{
	USED(c);
}

static long
raid0read(Chan *c, void *va, long count, vlong offset)
{
    int addr, o, n, r;
    int nread = 0;
	char data[count];
	int initcount = count, initoffset=offset;

	if(c->qid.type & QTDIR) //If file is a directory
		return devdirread(c, va, count, raid0tab, nelem(raid0tab), devgen);
	
	switch (c->qid.path) {
		case Qdata:
            if ((chan0 == nil) || (chan1 == nil)) {
                r = readstr(offset, va, count, "either of the channels are nil\n");
                break ; 
            }
            if (offset > raid0tab[Qdata].length) {
                return 0;
            }
            while (count > 0) {
                addr = offset / BLOCKSIZE; //which block
                o = offset%BLOCKSIZE; //offset in each block
                n = BLOCKSIZE - o; //number of bytes left in block to read
                if (n > count) 
                    n = count;
                if (addr%2 == 0) {
                    r = devtab[chan0->type]->read(chan0, data + nread, n, (addr >> 1)*BLOCKSIZE + o);                    
                } else {
                    r = devtab[chan1->type]->read(chan1, data + nread, n, (addr >> 1)*BLOCKSIZE + o );
                }
                count = count - n;
                nread = nread + n;
                offset = offset + n;
            }     
            r = readstr(initoffset, va, initcount, data);

            break;
		case Qctl:
			r = readstr(offset, va, count, "raid0: Read from control file\n");
            break;
	}

	return r;
}


static long
raid0write(Chan *c, void *va, long count, vlong offset)
{
	// USED(c);
	//USED(va);
	//USED(offset);

    Cmdbuf *cb;        
    Cmdtab *ct;
    char data[count];
    int nread = 0;

    int addr, r, n, o;

    switch(c->qid.path) {
        case Qctl:
            cb = parsecmd(va, count);
            ct = lookupcmd(cb, raid0cmd, nelem(raid0cmd));  
            switch (ct->index) {
                case CMbind:
                    print("Binded!\n");
                    chan0 = namec(cb->f[1], Aopen, ORDWR, 0);
                    chan1 = namec(cb->f[2], Aopen, ORDWR, 0);
            }     
            break;
        case Qdata:
            print("write: count, offset: %d, %d\n", count, offset);
            if ((chan1 == nil) || (chan0 == nil)) {
                print("either of the channels is nil!\n");
                break;
            }
            
            if (offset > raid0tab[Qdata].length) {
                offset = raid0tab[Qdata].length;
            }
            strcpy(data, (const char*)va);
            
            raid0tab[Qdata].length += count;
            
            while (count > 0){
                addr = offset/BLOCKSIZE;
                o = offset%BLOCKSIZE;
                n = BLOCKSIZE - o;
                if (n >count)
                    n = count;
                if (addr%2 == 0) {
                    r = devtab[chan0->type]->write(chan0, data + nread, n, (addr >> 1)*BLOCKSIZE + o);                    
                } else {
                    r = devtab[chan1->type]->write(chan1, data + nread, n, (addr >> 1)*BLOCKSIZE + o );
                }
                count = count - n;
                nread = nread + n;
                offset = offset + n;
            }
            break;        
    }	
    return r;
}

Dev raid0devtab = {
	'R',
	"raid0",

	devinit,
	raid0attach,
	raid0walk,
	raid0stat,
	raid0open,
	devcreate,
	raid0close,
	raid0read,
	devbread,
	raid0write,
	devbwrite,
	devremove,
	devwstat,
};
