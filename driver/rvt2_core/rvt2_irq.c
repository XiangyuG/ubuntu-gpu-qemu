// SPDX-License-Identifier: GPL-2.0
#include <linux/interrupt.h>
#include <linux/pci.h>
#include "rvt2_drv.h"

static irqreturn_t rvt2_irq_handler(int irq, void *data)
{
    struct rvt2_device *rdev = data;
    u32 irq_status;

    irq_status = rvt2_read(rdev, RVT2_REG_IRQ_STATUS);
    if (!irq_status)
        return IRQ_NONE;

    if (irq_status & (1 << 0))  /* COMPLETION */
        wake_up_all(&rdev->fence_wq);

    if (irq_status & (1 << 1)) {  /* FAULT */
        dev_err(&rdev->pdev->dev, "device fault detected\n");
        wake_up_all(&rdev->fence_wq);
    }

    return IRQ_HANDLED;
}

int rvt2_irq_init(struct rvt2_device *rdev)
{
    int ret, nvec;

    nvec = pci_alloc_irq_vectors(rdev->pdev, 1, 2,
                                 PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_INTX);
    if (nvec < 0) {
        dev_err(&rdev->pdev->dev,
                "pci_alloc_irq_vectors failed: %d\n", nvec);
        return nvec;
    }
    dev_info(&rdev->pdev->dev, "allocated %d IRQ vectors\n", nvec);

    ret = request_irq(pci_irq_vector(rdev->pdev, 0), rvt2_irq_handler,
                      0, "rvt2_completion", rdev);
    if (ret)
        goto err_free;

    if (nvec >= 2) {
        ret = request_irq(pci_irq_vector(rdev->pdev, 1), rvt2_irq_handler,
                          0, "rvt2_fault", rdev);
        if (ret) {
            free_irq(pci_irq_vector(rdev->pdev, 0), rdev);
            goto err_free;
        }
    }

    /* Unmask all interrupts */
    rvt2_write(rdev, RVT2_REG_IRQ_MASK, 0);

    return 0;

err_free:
    pci_free_irq_vectors(rdev->pdev);
    return ret;
}

void rvt2_irq_fini(struct rvt2_device *rdev)
{
    int nvec = pci_msix_vec_count(rdev->pdev);

    rvt2_write(rdev, RVT2_REG_IRQ_MASK, 0xFFFFFFFF);

    free_irq(pci_irq_vector(rdev->pdev, 0), rdev);
    if (nvec >= 2)
        free_irq(pci_irq_vector(rdev->pdev, 1), rdev);

    pci_free_irq_vectors(rdev->pdev);
}
