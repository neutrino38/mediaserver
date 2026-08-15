/**
 * guardedbuffer.h — un tampon dont le dernier octet touche une page interdite.
 *
 * Un parseur qui lit un octet de trop ne se voit pas : le tampon voisin dans le
 * tas contient presque toujours quelque chose, et le test passe. On veut le
 * contraire — que le débordement soit BRUYANT. Le tampon est donc placé en fin
 * de page, la page suivante étant mappée PROT_NONE : lire (ou écrire) ne
 * serait-ce qu'un octet après la fin du paquet déclenche un SIGSEGV.
 *
 * Comme un SIGSEGV emporte tout le processus, les tests qui s'en servent
 * appellent le parseur dans un EXPECT_EXIT/ASSERT_EXIT : gtest fork, le fils
 * meurt (ou sort avec 0), et seul CE test échoue.
 *
 *     GuardedBuffer buf(paquet, sizeof(paquet));
 *     EXPECT_EXIT({ Parse(buf.data(), buf.size()); _exit(0); },
 *                 ::testing::ExitedWithCode(0), "");
 */
#ifndef GUARDEDBUFFER_H
#define GUARDEDBUFFER_H

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

class GuardedBuffer
{
public:
	GuardedBuffer(const void* src, size_t size)
	{
		this->size = size;
		pageSize = (size_t)sysconf(_SC_PAGESIZE);
		//Nombre de pages accessibles, plus UNE page de garde derriere.
		pages = (size + pageSize - 1) / pageSize;
		if (pages == 0)
			pages = 1;
		total = (pages + 1) * pageSize;

		base = (unsigned char*)mmap(NULL, total, PROT_READ | PROT_WRITE,
		                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED)
		{
			base = NULL;
			ptr  = NULL;
			return;
		}
		//La derniere page devient interdite : c'est le filet.
		mprotect(base + pages * pageSize, pageSize, PROT_NONE);

		//Le paquet finit exactement au bord de la page de garde.
		ptr = base + pages * pageSize - size;
		if (size && src)
			memcpy(ptr, src, size);
	}

	~GuardedBuffer()
	{
		if (base)
			munmap(base, total);
	}

	GuardedBuffer(const GuardedBuffer&) = delete;
	GuardedBuffer& operator=(const GuardedBuffer&) = delete;

	unsigned char* data() const { return ptr; }
	size_t         Size() const { return size; }
	bool           IsValid() const { return ptr != NULL; }

private:
	unsigned char* base = NULL;
	unsigned char* ptr = NULL;
	size_t         size = 0;
	size_t         pageSize = 0;
	size_t         pages = 0;
	size_t         total = 0;
};

#endif /* GUARDEDBUFFER_H */
