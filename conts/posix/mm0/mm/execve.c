/*
 * Program execution
 *
 * Copyright (C) 2008 Bahadir Balban
 */
#include <l4lib/arch/syslib.h>
#include <l4lib/arch/syscalls.h>
#include <l4lib/ipcdefs.h>
#include <l4lib/types.h>
#include <l4/macros.h>
#include <l4/api/errno.h>
#include <lib/malloc.h>
#include <vm_area.h>
#include <syscalls.h>
#include <string.h>
#include <exec.h>
#include <file.h>
#include <user.h>
#include <task.h>
#include <exit.h>
#include <lib/elf/elf.h>
#include <init.h>
#include <stat.h>
#include <alloca.h>

/*
 * Probes and parses the low-level executable file format and creates a
 * generic execution description that can be used to run the task.
 */
int task_setup_from_executable(struct vm_file *vmfile, struct tcb *task,
			       struct exec_file_desc *efd)
{
	memset(efd, 0, sizeof(*efd));

	return elf_parse_executable(task, vmfile, efd);
}

int init_execve(char *filepath)
{
	unsigned long vnum, length;
	struct vm_file *vmfile;
	struct exec_file_desc efd;
	struct tcb *new_task;
	struct args_struct args, env;
	char *env_string = "pagerid=0";
	int err;

	struct task_ids ids = {
		.tid = TASK_ID_INVALID,
		.spid = TASK_ID_INVALID,
		.tgid = TASK_ID_INVALID,
	};

	/* Set up args_struct */
	args.argc = 1;
	args.argv = alloca(sizeof(args.argv));
	args.argv[0] = alloca(strlen(filepath) + 1);
	strncpy(args.argv[0], filepath, strlen(filepath) + 1);
	args.size = sizeof(args.argv) * args.argc + strlen(filepath) + 1;

	/* Set up environment */
	env.argc = 1;
	env.argv = alloca(sizeof(env.argv));
	env.argv[0] = alloca(strlen(env_string) + 1);
	strncpy(env.argv[0], env_string, strlen(env_string) + 1);
	env.size = sizeof(env.argv) + strlen(env_string) + 1;

	/* Get file info from vfs */
	if ((err = vfs_open_bypath(filepath, &vnum, &length)) < 0)
		return err;

	/* Create and get the file structure */
	if (IS_ERR(vmfile = do_open2(0, 0, vnum, length)))
		return (int)vmfile;

#if 0
	if ((fd = sys_open(self, filepath,
			   O_RDONLY, 0)) < 0) {
		printf("FATAL: Could not open file "
		       "to execute initial task.\n");
		BUG();
	}
#endif

	if (IS_ERR(new_task = task_create(0, &ids,
					  THREAD_NEW_SPACE,
					  TCB_NO_SHARING))) {
		vm_file_put(vmfile); /* FIXME: sys_close() ??? */
		return (int)new_task;
	}

	/* Fill and validate tcb memory segment markers from executable file */
	if ((err = task_setup_from_executable(vmfile, new_task, &efd)) < 0) {
		vm_file_put(vmfile);
		kfree(new_task);
		return err;
	}

	/* Map task's new segment markers as virtual memory regions */
	if ((err = task_mmap_segments(new_task, vmfile,
				      &efd, &args, &env)) < 0) {
		vm_file_put(vmfile);
		kfree(new_task);
		return err;
	}

	/* Set up task registers via exchange_registers() */
	task_setup_registers(new_task, 0,
			     new_task->args_start,
			     new_task->pagerid);


	/* Add new task to global list */
	global_add_task(new_task);

	/* Start the task */
	task_start(new_task);

	return 0;
}


int do_execve(struct tcb *sender, char *filename, struct args_struct *args,
	      struct args_struct *env)
{
	unsigned long vnum, length;
	struct vm_file *vmfile;
	struct exec_file_desc efd;
	struct tcb *new_task, *tgleader;
	int err;

	/* Get file info from vfs */
	if ((err = vfs_open_bypath(filename, &vnum, &length)) < 0)
		return err;

	/* Create and get the file structure */
	if (IS_ERR(vmfile = do_open2(0, 0, vnum, length)))
		return (int)vmfile;

	/* Create a new tcb */
	if (IS_ERR(new_task = tcb_alloc_init(TCB_NO_SHARING))) {
		vm_file_put(vmfile);
		return (int)new_task;
	}

	/* Fill and validate tcb memory segment markers from executable file */
	if ((err = task_setup_from_executable(vmfile, new_task, &efd)) < 0) {
		vm_file_put(vmfile);
		kfree(new_task);
		return err;
	}

	/*
	 * If sender is a thread in a group, need to find the
	 * group leader and destroy all threaded children in
	 * the group.
	 */
	if (sender->clone_flags & TCB_SHARED_TGROUP) {
		struct tcb *thread;

		/* Find the thread group leader of sender */
		BUG_ON(!(tgleader = find_task(sender->tgid)));

		/* Destroy all children threads. */
		list_foreach_struct(thread, &tgleader->children, child_ref)
			do_exit(thread, 0);
	} else {
		/* Otherwise group leader is same as sender */
		tgleader = sender;
	}

	/*
	 * Copy data to be retained from exec'ing task to new one.
	 * Release all task resources, do everything done in
	 * exit() except destroying the actual thread.
	 */
	if ((err = execve_recycle_task(new_task, tgleader)) < 0) {
		vm_file_put(vmfile);
		kfree(new_task);
		return err;
	}

	/* Map task's new segment markers as virtual memory regions */
	if ((err = task_mmap_segments(new_task, vmfile, &efd, args, env)) < 0) {
		vm_file_put(vmfile);
		kfree(new_task);
		return err;
	}

	/* Set up task registers via exchange_registers() */
	task_setup_registers(new_task, 0, new_task->args_start, new_task->pagerid);

	/* Add new task to global list */
	global_add_task(new_task);

	/* Start the task */
	task_start(new_task);

#if 0
TODO:
Dynamic Linking.

	/* See if an interpreter (dynamic linker) is needed */

	/* Find the interpreter executable file, if needed */

	/*
	 * Map all dynamic linker file segments
	 * (should not clash with original executable
	 */

	/* Set up registers to run dynamic linker (exchange_registers()) */

	/* Run the interpreter */

	/*
	 * The interpreter will:
	 * - Need some initial info (dyn sym tables) at a certain location
	 * - Find necessary shared library files in userspace
	 *   (will use open/read).
	 * - Map them into process address space via mmap()
	 * - Reinitialise references to symbols in the shared libraries
	 * - Jump to the entry point of main executable.
	 */
#endif
	return 0;
}

/*
 * Copy from one buffer to another. Stop if maxlength or
 * a page boundary is hit.
 */
int strncpy_page(void *to_ptr, void *from_ptr, int maxlength)
{
	int count = 0;
	char *to = to_ptr, *from = from_ptr;

	do {
		if ((to[count] = from[count]) == '\0') {
			count++;
			break;
		} else
			count++;
	} while (count < maxlength && !page_boundary(&from[count]));

	if (page_boundary(&from[count]))
		return -EFAULT;
	if (count == maxlength)
		return -E2BIG;

	return count;
}

/*
 * Copy from one buffer to another. Stop if maxlength or
 * a page boundary is hit. Breaks if unsigned long sized copy value is 0,
 * as opposed to a 0 byte as in string copy. If byte size 0 was used
 * a valid pointer with a 0 byte in it would give a false termination.
 */
int bufncpy_page(void *to_ptr, void *from_ptr, int maxlength)
{
	int count = 0;
	unsigned long *to = to_ptr, *from = from_ptr;

	do {
		if ((to[count] = from[count]) == 0) {
			count++;
			break;
		} else
			count++;
	} while (count < maxlength && !page_boundary(&from[count]));

	if (page_boundary(&from[count]))
		return -EFAULT;
	if (count == maxlength)
		return -E2BIG;

	return count;
}

/*
 * Copies a variable sized userspace string or array of pointers
 * (think &argv[0]), into buffer. If a page boundary is hit,
 * unmaps the previous page, validates and maps the new page.
 */
int copy_user_buf(struct tcb *task, void *buf, char *user, int maxlength,
		  int elem_size)
{
	int count = maxlength;
	int copied = 0, ret = 0, total = 0;
	void *mapped = 0;
	int (*copy_func)(void *, void *, int count);

	/* This bit determines what size copier function to use. */
	if (elem_size == sizeof(char))
		copy_func = strncpy_page;
	else if (elem_size == sizeof(unsigned long))
		copy_func = bufncpy_page;
	else
		return -EINVAL;

	/* Map the first page the user buffer is in */
	if (!(mapped = pager_validate_map_user_range(task, user,
						     TILL_PAGE_ENDS(user),
						     VM_READ)))
		return -EINVAL;

	while ((ret = copy_func(buf + copied, mapped, count)) < 0) {
		if (ret == -E2BIG)
			return ret;
		else if (ret == -EFAULT) {
			/*
			 * Copied is always in bytes no matter what elem_size is
			 * because we know we hit a page boundary and we increase
			 * by the page boundary bytes
			 */
			pager_unmap_user_range(mapped, TILL_PAGE_ENDS(mapped));
			copied += TILL_PAGE_ENDS(mapped);
			count -= TILL_PAGE_ENDS(mapped);
			if (!(mapped =
			      pager_validate_map_user_range(task, user + copied,
							    TILL_PAGE_ENDS(user + copied),
							    VM_READ)))
				return -EINVAL;
		}
	}

	/* Note copied is always in bytes */
	total = (copied / elem_size) + ret;

	/* Unmap the final page */
	pager_unmap_user_range(mapped, TILL_PAGE_ENDS(mapped));

	return total;
}

/*
 * Calls copy_user_buf with char-sized copying. This matters because
 * buffer is variable and the terminator must be in char size
 */
static inline int
copy_user_string(struct tcb *task, void *buf, char *user,
		 int maxlength)
{
	return copy_user_buf(task, buf, user, maxlength, sizeof(char));
}

/*
 * Calls copy_user_buf with unsigned long sized copying. This matters
 * because buffer is variable and the terminator must be in ulong size
 */
static inline int
copy_user_ptrs(struct tcb *task, void *buf, char *user,
		 int maxlength)
{
	return copy_user_buf(task, buf, user, maxlength, sizeof(unsigned long));
}


int copy_user_args(struct tcb *task, struct args_struct *args,
		   void *argv_user, int args_max)
{
	char **argv = 0;
	void *argsbuf;
	char *curbuf;
	int argc = 0;
	int used;
	int count;

	if (!(argsbuf = kzalloc(args_max)))
		return -ENOMEM;

	/*
	 * First, copy the null-terminated array of
	 * pointers to argument strings.
	 */
	if ((count = copy_user_ptrs(task, argsbuf, argv_user, args_max)) < 0)
		goto out;

	/* On success, we get the number of arg strings + the terminator */
	argc = count - 1;
	used = count * sizeof(char *);
	argv = argsbuf;
	curbuf = argsbuf + used;

	/* Now we copy each argument string into buffer */
	for (int i = 0; i < argc; i++) {
		/* Copy string into empty space in buffer */
		if ((count = copy_user_string(task, curbuf, argv[i],
					      args_max - used)) < 0)
			goto out;

		/* Replace pointer to string with copied location */
		argv[i] = curbuf;

		/* Update current empty buffer location */
		curbuf += count;

		/* Increase used buffer count */
		used += count;
	}

	/* Set up the args struct */
	args->argc = argc;
	args->argv = argv;
	args->size = used;

	return 0;

out:
	kfree(argsbuf);
	return count;
}

int sys_execve(struct tcb *sender, char *pathname, char *argv[], char *envp[])
{
	int ret;
	char *path;
	struct args_struct args;
	struct args_struct env;

	if (!(path = kzalloc(PATH_MAX)))
		return -ENOMEM;

	memset(&args, 0, sizeof(args));
	memset(&env, 0, sizeof(env));

	/* Copy the executable path string */
	if ((ret = copy_user_string(sender, path, pathname, PATH_MAX)) < 0)
		return ret;
	// printf("%s: Copied pathname: %s\n", __FUNCTION__, path);

	/* Copy the args */
	if (argv && ((ret = copy_user_args(sender, &args, argv, ARGS_MAX)) < 0))
		goto out1;

	/* Copy the env */
	if (envp && ((ret = copy_user_args(sender, &env, envp,
					   ARGS_MAX - args.size)) < 0))
		goto out2;

	ret = do_execve(sender, path, &args, &env);

	if (env.argv)
		kfree(env.argv);
out2:
	if (args.argv)
		kfree(args.argv);
out1:
	kfree(path);
	return ret;
}
