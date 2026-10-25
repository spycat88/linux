// SPDX-License-Identifier: GPL-2.0-only
/*
 * QSEECOM apps reached through QTEE's QSEECom compat interface.
 *
 * On some chips UEFI loads its trusted apps, uefisecapp among them, through
 * QTEE's QSEECom compat app loader over SMCInvoke. QTEE's legacy QSEECOM app
 * lookup doesn't find apps loaded that way, so qcom_qseecom offers no
 * uefisecapp client, and QTEE may also refuse the app's object-IPC service to
 * Linux. The compat app loader still finds the app by name. This offers it as
 * a qseecom_client whose requests go through IQSEEComCompat_sendRequest, so
 * the QSEECOM uefisecapp driver can use it unchanged.
 *
 * Releasing the last reference to an app's IQSEEComCompat object unloads the
 * app from QTEE, and only UEFI loads uefisecapp, so the client keeps the
 * object for as long as it exists.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/auxiliary_bus.h>
#include <linux/firmware/qcom/qcom_qseecom.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "qcomtee.h"

/* CQSEEComCompatAppLoader and its lookupTA operation. */
#define QCOMTEE_COMPAT_APP_LOADER_UID		122
#define QCOMTEE_COMPAT_APP_LOADER_OP_LOOKUP_TA	2
/* IQSEEComCompat's sendRequest operation. */
#define QCOMTEE_COMPAT_OP_SEND_REQUEST		0
/* The architecture lookupTA reports for a 64-bit app (ELFCLASS64). */
#define QCOMTEE_COMPAT_ARCH_64			2

#define QCOMTEE_COMPAT_UEFISECAPP		"qcom.tz.uefisecapp"

/**
 * struct qcomtee_compat_client - QSEECOM client for an app behind the compat
 * app loader.
 * @client: client the QSEECOM app drivers bind to.
 * @ctx: TEE context for the invocations.
 * @app: the app's IQSEEComCompat object.
 * @is64: the app is 64-bit.
 */
struct qcomtee_compat_client {
	struct qseecom_client client;
	struct tee_context *ctx;
	struct qcomtee_object *app;
	u32 is64;
};

/* qseecom_client.send: IQSEEComCompat_sendRequest with no embedded buffers. */
static int qcomtee_compat_send(struct qseecom_client *client, void *req,
			       size_t req_size, void *rsp, size_t rsp_size)
{
	struct qcomtee_compat_client *compat =
		container_of(client, struct qcomtee_compat_client, client);
	struct qcomtee_arg u[11] = { 0 };
	int i, ret, result;

	struct qcomtee_object_invoke_ctx *oic __free(kfree) =
		qcomtee_object_invoke_ctx_alloc(compat->ctx, true);
	if (!oic)
		return -ENOMEM;

	/* IB: request, response, embedded buffer offsets (none) and is64. */
	u[0].type = QCOMTEE_ARG_TYPE_IB;
	u[0].b.addr = req;
	u[0].b.size = req_size;
	u[1].type = QCOMTEE_ARG_TYPE_IB;
	u[1].b.addr = rsp;
	u[1].b.size = rsp_size;
	u[2].type = QCOMTEE_ARG_TYPE_IB;
	u[3].type = QCOMTEE_ARG_TYPE_IB;
	u[3].b.addr = &compat->is64;
	u[3].b.size = sizeof(compat->is64);
	/* OB: the request and response as the app leaves them. */
	u[4].type = QCOMTEE_ARG_TYPE_OB;
	u[4].b.addr = req;
	u[4].b.size = req_size;
	u[5].type = QCOMTEE_ARG_TYPE_OB;
	u[5].b.addr = rsp;
	u[5].b.size = rsp_size;
	/* IO: four shared memory objects, none used. */
	for (i = 6; i < 10; i++) {
		u[i].type = QCOMTEE_ARG_TYPE_IO;
		u[i].o = NULL_QCOMTEE_OBJECT;
	}

	ret = qcomtee_object_do_invoke(oic, compat->app,
				       QCOMTEE_COMPAT_OP_SEND_REQUEST, u,
				       &result);
	if (ret)
		return ret;
	if (result) {
		dev_dbg(&client->aux_dev.dev, "sendRequest failed (%d)\n",
			result);
		return -EIO;
	}

	return 0;
}

/* Looks up the app @name through the compat app loader. */
static struct qcomtee_object *
qcomtee_compat_lookup(struct qcomtee_object_invoke_ctx *oic,
		      struct qcomtee_object *client_env, const char *name,
		      u32 *arch)
{
	struct qcomtee_object *loader, *app = NULL_QCOMTEE_OBJECT;
	struct qcomtee_arg u[4] = { 0 };
	int ret, result = 0;

	loader = qcomtee_object_get_service(oic, client_env,
					    QCOMTEE_COMPAT_APP_LOADER_UID,
					    &result);
	if (loader == NULL_QCOMTEE_OBJECT) {
		pr_debug("QSEECom compat app loader unavailable (%d)\n",
			 result);
		return NULL_QCOMTEE_OBJECT;
	}

	u[0].type = QCOMTEE_ARG_TYPE_IB;
	u[0].b.addr = (void *)name;
	u[0].b.size = strlen(name);
	u[1].type = QCOMTEE_ARG_TYPE_OB;
	u[1].b.addr = arch;
	u[1].b.size = sizeof(*arch);
	u[2].type = QCOMTEE_ARG_TYPE_OO;
	ret = qcomtee_object_do_invoke(oic, loader,
				       QCOMTEE_COMPAT_APP_LOADER_OP_LOOKUP_TA,
				       u, &result);
	if (!ret && !result)
		app = u[2].o;
	else
		pr_debug("%s not found by the compat app loader (%d, %d)\n",
			 name, ret, result);

	qcomtee_object_put(loader);
	return app;
}

static void qcomtee_compat_release(struct device *dev)
{
	struct qcomtee_compat_client *compat =
		container_of(dev, struct qcomtee_compat_client,
			     client.aux_dev.dev);

	qcomtee_object_put(compat->app);
	kfree(compat);
}

/**
 * qcomtee_compat_register() - Offer uefisecapp as a QSEECOM client when only
 * the compat app loader reaches it.
 * @qcomtee: the QTEE driver.
 * @parent: device the client goes under.
 *
 * Does nothing when QTEE's legacy QSEECOM lookup finds uefisecapp, which
 * qcom_qseecom then serves, when QTEE lets Linux open uefisecapp's own
 * service, which the TEE uefisecapp driver uses, or when the compat app loader
 * doesn't find it.
 */
void qcomtee_compat_register(struct qcomtee *qcomtee, struct device *parent)
{
	struct qcomtee_object *client_env, *service, *app;
	struct qcomtee_compat_client *compat;
	int result = 0;
	u32 arch = 0;
	u32 app_id;

	if (!IS_ENABLED(CONFIG_QCOM_QSEECOM_UEFISECAPP))
		return;

	if (!qcom_scm_qseecom_app_get_id(QCOMTEE_COMPAT_UEFISECAPP, &app_id))
		return;

	struct qcomtee_object_invoke_ctx *oic __free(kfree) =
		qcomtee_object_invoke_ctx_alloc(qcomtee->ctx, true);
	if (!oic)
		return;

	client_env = qcomtee_object_get_client_env(oic);
	if (client_env == NULL_QCOMTEE_OBJECT)
		return;

	service = qcomtee_object_get_service(oic, client_env,
					     QCOMTEE_UEFI_SEC_UID, &result);
	if (service != NULL_QCOMTEE_OBJECT) {
		qcomtee_object_put(service);
		app = NULL_QCOMTEE_OBJECT;
	} else {
		app = qcomtee_compat_lookup(oic, client_env,
					    QCOMTEE_COMPAT_UEFISECAPP, &arch);
	}
	qcomtee_object_put(client_env);
	if (app == NULL_QCOMTEE_OBJECT)
		return;

	compat = kzalloc_obj(*compat);
	if (!compat) {
		qcomtee_object_put(app);
		return;
	}
	compat->ctx = qcomtee->ctx;
	compat->app = app;
	compat->is64 = arch == QCOMTEE_COMPAT_ARCH_64;
	compat->client.send = qcomtee_compat_send;
	compat->client.aux_dev.name = "uefisecapp";
	compat->client.aux_dev.dev.parent = parent;
	compat->client.aux_dev.dev.release = qcomtee_compat_release;

	if (auxiliary_device_init(&compat->client.aux_dev)) {
		qcomtee_object_put(app);
		kfree(compat);
		return;
	}
	if (auxiliary_device_add(&compat->client.aux_dev)) {
		auxiliary_device_uninit(&compat->client.aux_dev);
		return;
	}

	qcomtee->compat_uefisecapp = compat;
	pr_info("%s reached through the QSEECom compat app loader\n",
		QCOMTEE_COMPAT_UEFISECAPP);
}

/**
 * qcomtee_compat_unregister() - Remove the client qcomtee_compat_register()
 * offered.
 * @qcomtee: the QTEE driver.
 */
void qcomtee_compat_unregister(struct qcomtee *qcomtee)
{
	struct qcomtee_compat_client *compat = qcomtee->compat_uefisecapp;

	if (!compat)
		return;

	qcomtee->compat_uefisecapp = NULL;
	auxiliary_device_delete(&compat->client.aux_dev);
	auxiliary_device_uninit(&compat->client.aux_dev);
}
