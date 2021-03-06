#include "udev_if.h"
#include "joystick_state.h"
#include "pnet_if.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include "timespec_util.h"

extern bool volatile g_terminate;

udev_if::udev_if(const char *sysname) : m_sysname(sysname) {
  m_udev = udev_new();
  if (!m_udev) {
    std::cerr << "error calling udev_new(): " << std::strerror(errno)
              << std::endl;
    return;
  }

  /* Set up monitor to handle device reconnections */
  m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
  if (!m_monitor) {
    std::cerr << "error calling udev_monitor_new_from_netlink(): "
              << std::strerror(errno) << std::endl;
    return;
  }

  udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "input", nullptr);
  udev_monitor_filter_update(m_monitor);
  udev_monitor_enable_receiving(m_monitor);
  m_monitor_fd = udev_monitor_get_fd(m_monitor);

  /* Set up device if initially connected */
  if (udev_device_ptr dev =
          udev_device_new_from_subsystem_sysname(m_udev, "input", sysname)) {
    const char *devnode = udev_device_get_devnode(dev);
    m_joystick_fd = ::open(devnode, O_RDONLY | O_NONBLOCK);
    if (m_joystick_fd == -1) {
      std::cerr << "error opening " << devnode
                << " for reading: " << std::strerror(errno) << std::endl;
    } else {
      std::cout << "Opened " << devnode << std::endl;
    }
  }
}

#define return_if_interrupted                                                  \
  if (errno == EINTR)                                                          \
  return false

template <typename A> bool udev_if::handle_udev_monitor(A on_add) {
  while (udev_device_ptr dev = udev_monitor_receive_device(m_monitor)) {
    const char *sysname = udev_device_get_sysname(dev);
    if (!std::strcmp(sysname, m_sysname)) {
      const char *action = udev_device_get_action(dev);
      if (!std::strcmp(action, "add")) {
        on_add();
        const char *devnode = udev_device_get_devnode(dev);
        if (m_joystick_fd.close())
          return_if_interrupted;
        m_joystick_fd = ::open(devnode, O_RDONLY | O_NONBLOCK);
        if (m_joystick_fd == -1) {
          return_if_interrupted;
          std::cerr << "error opening " << devnode
                    << " for reading: " << std::strerror(errno) << std::endl;
        } else {
          std::cout << "Opened " << devnode << std::endl;
        }
      }
    }
  }
  return_if_interrupted;

  return true;
}

template <typename E, typename L>
bool udev_if::handle_joystick(E on_event, L on_lost) {
  js_event event;
  while (m_joystick_fd.read_event(event)) {
    on_event(event);
  }
  return_if_interrupted;
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    on_lost();
    std::cout << "Lost Joystick" << std::endl;
    m_joystick_fd.close();
  }
  return true;
}

#undef return_if_interrupted
#define return_if_interrupted                                                  \
  if (errno == EINTR)                                                          \
  return 0

int udev_if::event_loop(joystick_state &state, pnet_if &pnet,
                        uint32_t periodic_us) {
  /* Attempt to use real-time scheduling */
  static_assert(_POSIX_THREAD_PRIORITY_SCHEDULING > 0,
                "SCHED_FIFO not available");
  struct sched_param param = {.sched_priority = 5};
  if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
    std::cerr << "Unable to setup SCHED_FIFO: " << strerror(errno) << std::endl;
  }

  const timespec periodic_ts{0, periodic_us * 1000};
  timespec next_update{};
  auto set_next_update = [&]() {
    timespec cur_time{};
    clock_gettime(CLOCK_MONOTONIC, &cur_time);
    timespec_add(next_update, cur_time, periodic_ts);
  };
  set_next_update();

  while (!g_terminate) {
    /*
     * Wait on one of two event sources:
     * - udev monitor for controller reconnections
     * - device file for controller events
     */
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(m_monitor_fd, &read_fds);
    int max_fd = m_monitor_fd;
    if (m_joystick_fd != -1) {
      FD_SET(m_joystick_fd, &read_fds);
      max_fd = std::max(max_fd, int(m_joystick_fd));
    }
    timespec cur_time{};
    clock_gettime(CLOCK_MONOTONIC, &cur_time);
    timespec rem_time{};
    int n_fd_trig = -1;
    if (timespec_subtract(rem_time, next_update, cur_time) ||
        (n_fd_trig = pselect(max_fd + 1, &read_fds, nullptr, nullptr, &rem_time,
                             nullptr)) == 0) {
      /* timeout occurred - handle periodic updates */
      set_next_update();
      if (pnet.periodic())
        return 1;
      return_if_interrupted;
      continue;
    }
    if (n_fd_trig == -1) {
      return_if_interrupted;
      std::cerr << "error calling pselect(): " << std::strerror(errno)
                << std::endl;
      continue;
    }

    bool js_update = false;

    /* Handle udev monitor events */
    if (FD_ISSET(m_monitor_fd, &read_fds)) {
      if (!handle_udev_monitor([&]() {
            state.reset();
            js_update = true;
          }))
        return 0;
    }

    /* Handle joystick events */
    if (m_joystick_fd != -1 && FD_ISSET(m_joystick_fd, &read_fds)) {
      if (!handle_joystick(
              [&](const js_event &event) {
                state.receive_event(event);
                js_update = true;
              },
              [&]() {
                state.reset();
                js_update = true;
              }))
        return 0;
    }

    if (js_update) {
      pnet.send_updates(state);
      return_if_interrupted;
    }
  }

  return 0;
}
