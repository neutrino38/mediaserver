#ifndef _TASK_H_
#define _TASK_H_

// Etat general d'une tache/thread du mcu.
// Isole ici (auparavant en fin de tools.h) lors de la consolidation de tools.h
// vers libmedkit : cet enum est propre au mcu (audiostream/videostream/
// textstream) et n'a pas sa place dans medkit/tools.h.
enum TaskState
{
    TaskIdle = 0,
    TaskStarting = 1,
    TaskRunning = 2,
    TaskStopping = 3
};

#endif /* _TASK_H_ */
