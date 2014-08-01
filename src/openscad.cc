/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "openscad.h"
#include "node.h"
#include "module.h"
#include "modcontext.h"
#include "value.h"
#include "export.h"
#include "builtin.h"
#include "printutils.h"
#include "handle_dep.h"
#include "feature.h"
#include "parsersettings.h"
#include "rendersettings.h"
#include "PlatformUtils.h"
#include "LibraryInfo.h"
#include "nodedumper.h"
#include "CocoaUtils.h"

#include <string>
#include <vector>
#include <fstream>

#ifdef ENABLE_CGAL
#include "CGAL_Nef_polyhedron.h"
#endif

#include "csgterm.h"
#include "CSGTermEvaluator.h"
#include "CsgInfo.h"

#include <sstream>

#ifdef __APPLE__
#include "AppleEvents.h"
#ifdef OPENSCAD_DEPLOY
  #include "SparkleAutoUpdater.h"
#endif
#endif

#include "Camera.h"
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include "boosty.h"

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace Render { enum type { CGAL, OPENCSG, THROWNTOGETHER }; };
std::string commandline_commands;
std::string currentdir;
using std::string;
using std::vector;
using boost::lexical_cast;
using boost::is_any_of;

class Echostream : public std::ofstream
{
public:
	Echostream( const char * filename ) : std::ofstream( filename ) {
		set_output_handler( &Echostream::output, this );
	}
	static void output( const std::string &msg, void *userdata ) {
		Echostream *thisp = static_cast<Echostream*>(userdata);
		*thisp << msg << "\n";
	}
	~Echostream() {
		this->close();
	}
};

static void help(const char *progname)
{
  int tablen = strlen(progname)+8;
  char tabstr[tablen+1];
  for (int i=0;i<tablen;i++) tabstr[i] = ' ';
  tabstr[tablen] = '\0';

	PRINTB("Usage: %1% [ -o output_file [ -d deps_file ] ]\\\n"
         "%2%[ -m make_command ] [ -D var=val [..] ] \\\n"
         "%2%[ --version ] [ --info ] \\\n"
         "%2%[ --camera=translatex,y,z,rotx,y,z,dist | \\\n"
         "%2%  --camera=eyex,y,z,centerx,y,z ] \\\n"
         "%2%[ --autocenter ] \\\n"
         "%2%[ --viewall ] \\\n"
         "%2%[ --imgsize=width,height ] [ --projection=(o)rtho|(p)ersp] \\\n"
         "%2%[ --render | --preview[=throwntogether] ] \\\n"
         "%2%[ --csglimit=num ]"
#ifdef ENABLE_EXPERIMENTAL
         " [ --enable=<feature> ]"
#endif
         "\\\n"
#ifdef DEBUG
				 "%2%[ --debug=module ] \\\n"
#endif
         "%2%filename\n",
 				 progname % (const char *)tabstr);
	exit(1);
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
static void version()
{
	PRINTB("OpenSCAD version %s\n", TOSTRING(OPENSCAD_VERSION));
	exit(1);
}

static void info()
{
	std::cout << LibraryInfo::info() << "\n\n";

	CsgInfo csgInfo = CsgInfo();
	try {
		csgInfo.glview = new OffscreenView(512,512);
	} catch (int error) {
		PRINTB("Can't create OpenGL OffscreenView. Code: %i. Exiting.\n", error);
		exit(1);
	}

	std::cout << csgInfo.glview->getRendererInfo() << "\n";

	exit(0);
}

Camera get_camera( po::variables_map vm )
{
	Camera camera;

	if (vm.count("camera")) {
		vector<string> strs;
		vector<double> cam_parameters;
		split(strs, vm["camera"].as<string>(), is_any_of(","));
		if ( strs.size()==6 || strs.size()==7 ) {
			BOOST_FOREACH(string &s, strs)
				cam_parameters.push_back(lexical_cast<double>(s));
			camera.setup( cam_parameters );
		} else {
			PRINT("Camera setup requires either 7 numbers for Gimbal Camera\n");
			PRINT("or 6 numbers for Vector Camera\n");
			exit(1);
		}
	}

	if (camera.type == Camera::GIMBAL) {
		camera.gimbalDefaultTranslate();
	}

	if (vm.count("viewall")) {
		camera.viewall = true;
	}

	if (vm.count("autocenter")) {
		camera.autocenter = true;
	}

	if (vm.count("projection")) {
		string proj = vm["projection"].as<string>();
		if (proj=="o" || proj=="ortho" || proj=="orthogonal")
			camera.projection = Camera::ORTHOGONAL;
		else if (proj=="p" || proj=="perspective")
			camera.projection = Camera::PERSPECTIVE;
		else {
			PRINT("projection needs to be 'o' or 'p' for ortho or perspective\n");
			exit(1);
		}
	}

	int w = RenderSettings::inst()->img_width;
	int h = RenderSettings::inst()->img_height;
	if (vm.count("imgsize")) {
		vector<string> strs;
		split(strs, vm["imgsize"].as<string>(), is_any_of(","));
		if ( strs.size() != 2 ) {
			PRINT("Need 2 numbers for imgsize\n");
			exit(1);
		} else {
			w = lexical_cast<int>( strs[0] );
			h = lexical_cast<int>( strs[1] );
		}
	}
	camera.pixel_width = w;
	camera.pixel_height = h;

	return camera;
}

#ifdef OPENSCAD_TESTING
#undef OPENSCAD_QTGUI
#else
#define OPENSCAD_QTGUI 1
#include <QApplication>
#endif

static bool checkAndExport(shared_ptr<const Geometry> root_geom, unsigned nd,
	enum FileFormat format, const char *filename)
{
	if (root_geom->getDimension() != nd) {
		PRINTB("Current top level object is not a %dD object.", nd);
		return false;
	}
	exportFileByName(root_geom.get(), format, filename, filename);
	return true;
}

int cmdline(const char *deps_output_file, const std::string &filename, Camera &camera, const char *output_file, const fs::path &original_path, Render::type renderer, int argc, char ** argv )
{
#ifdef OPENSCAD_QTGUI
	QCoreApplication app(argc, argv);
	const std::string application_path = QApplication::instance()->applicationDirPath().toLocal8Bit().constData();
#else
	const std::string application_path = boosty::stringy(boosty::absolute(boost::filesystem::path(argv[0]).parent_path()));
#endif
	parser_init(application_path);
	Tree tree;
#ifdef ENABLE_CGAL
	GeometryEvaluator geomevaluator(tree);
#endif
	const char *stl_output_file = NULL;
	const char *off_output_file = NULL;
	const char *amf_output_file = NULL;
	const char *dxf_output_file = NULL;
	const char *svg_output_file = NULL;
	const char *csg_output_file = NULL;
	const char *png_output_file = NULL;
	const char *ast_output_file = NULL;
	const char *term_output_file = NULL;
	const char *echo_output_file = NULL;

	std::string suffix = boosty::extension_str( output_file );
	boost::algorithm::to_lower( suffix );

	if (suffix == ".stl") stl_output_file = output_file;
	else if (suffix == ".off") off_output_file = output_file;
	else if (suffix == ".amf") amf_output_file = output_file;
	else if (suffix == ".dxf") dxf_output_file = output_file;
	else if (suffix == ".svg") svg_output_file = output_file;
	else if (suffix == ".csg") csg_output_file = output_file;
	else if (suffix == ".png") png_output_file = output_file;
	else if (suffix == ".ast") ast_output_file = output_file;
	else if (suffix == ".term") term_output_file = output_file;
	else if (suffix == ".echo") echo_output_file = output_file;
	else {
		PRINTB("Unknown suffix for output file %s\n", output_file);
		return 1;
	}

	// Top context - this context only holds builtins
	ModuleContext top_ctx;
	top_ctx.registerBuiltin();
#ifdef DEBUG
	PRINTDB("Top ModuleContext:\n%s",top_ctx.dump(NULL, NULL));
#endif
	shared_ptr<Echostream> echostream;
	if (echo_output_file)
		echostream.reset( new Echostream( echo_output_file ) );

	FileModule *root_module;
	ModuleInstantiation root_inst("group");
	AbstractNode *root_node;
	AbstractNode *absolute_root_node;
	shared_ptr<const Geometry> root_geom;

	handle_dep(filename.c_str());

	std::ifstream ifs(filename.c_str());
	if (!ifs.is_open()) {
		PRINTB("Can't open input file '%s'!\n", filename.c_str());
		return 1;
	}
	std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
	text += "\n" + commandline_commands;
	fs::path abspath = boosty::absolute(filename);
	std::string parentpath = boosty::stringy(abspath.parent_path());
	root_module = parse(text.c_str(), parentpath.c_str(), false);
	if (!root_module) {
		PRINTB("Can't parse file '%s'!\n", filename.c_str());
		return 1;
	}
	root_module->handleDependencies();

	fs::path fpath = boosty::absolute(fs::path(filename));
	fs::path fparent = fpath.parent_path();
	fs::current_path(fparent);
	top_ctx.setDocumentPath(fparent.string());

	AbstractNode::resetIndexCounter();
	absolute_root_node = root_module->instantiate(&top_ctx, &root_inst, NULL);

	// Do we have an explicit root node (! modifier)?
	if (!(root_node = find_root_tag(absolute_root_node)))
		root_node = absolute_root_node;

	tree.setRoot(root_node);

	if (csg_output_file) {
		fs::current_path(original_path);
		std::ofstream fstream(csg_output_file);
		if (!fstream.is_open()) {
			PRINTB("Can't open file \"%s\" for export", csg_output_file);
		}
		else {
			fs::current_path(fparent); // Force exported filenames to be relative to document path
			fstream << tree.getString(*root_node) << "\n";
			fstream.close();
		}
	}
	else if (ast_output_file) {
		fs::current_path(original_path);
		std::ofstream fstream(ast_output_file);
		if (!fstream.is_open()) {
			PRINTB("Can't open file \"%s\" for export", ast_output_file);
		}
		else {
			fs::current_path(fparent); // Force exported filenames to be relative to document path
			fstream << root_module->dump("", "") << "\n";
			fstream.close();
		}
	}
	else if (term_output_file) {
		std::vector<shared_ptr<CSGTerm> > highlight_terms;
		std::vector<shared_ptr<CSGTerm> > background_terms;

		CSGTermEvaluator csgRenderer(tree);
		shared_ptr<CSGTerm> root_raw_term = csgRenderer.evaluateCSGTerm(*root_node, highlight_terms, background_terms);

		fs::current_path(original_path);
		std::ofstream fstream(term_output_file);
		if (!fstream.is_open()) {
			PRINTB("Can't open file \"%s\" for export", term_output_file);
		}
		else {
			if (!root_raw_term)
				fstream << "No top-level CSG object\n";
			else {
				fstream << root_raw_term->dump() << "\n";
			}
			fstream.close();
		}
	}
	else {
#ifdef ENABLE_CGAL
		if ((echo_output_file || png_output_file) && !(renderer==Render::CGAL)) {
			// echo or OpenCSG png -> don't necessarily need CGALMesh evaluation
		} else {
			root_geom = geomevaluator.evaluateGeometry(*tree.root(), true);
			if (!root_geom) {
				PRINT("No top-level object found.");
				return 1;
			}
			const CGAL_Nef_polyhedron *N = dynamic_cast<const CGAL_Nef_polyhedron*>(root_geom.get());
		}

		fs::current_path(original_path);

		if (deps_output_file) {
			std::string deps_out( deps_output_file );
			std::string geom_out;
			if ( stl_output_file ) geom_out = std::string(stl_output_file);
			else if ( off_output_file ) geom_out = std::string(off_output_file);
			else if ( amf_output_file ) geom_out = std::string(amf_output_file);
			else if ( dxf_output_file ) geom_out = std::string(dxf_output_file);
			else if ( svg_output_file ) geom_out = std::string(svg_output_file);
			else if ( png_output_file ) geom_out = std::string(png_output_file);
			else {
				PRINTB("Output file:%s\n",output_file);
				PRINT("Sorry, don't know how to write deps for that file type. Exiting\n");
				return 1;
			}
			int result = write_deps( deps_out, geom_out );
			if ( !result ) {
				PRINT("error writing deps");
				return 1;
			}
		}

		if (stl_output_file) {
			if (!checkAndExport(root_geom, 3, OPENSCAD_STL, stl_output_file))
				return 1;
		}

		if (off_output_file) {
			if (!checkAndExport(root_geom, 3, OPENSCAD_OFF, off_output_file))
				return 1;
		}

		if (amf_output_file) {
			if (!checkAndExport(root_geom, 3, OPENSCAD_AMF, amf_output_file))
				return 1;
		}

		if (dxf_output_file) {
			if (!checkAndExport(root_geom, 2, OPENSCAD_DXF, dxf_output_file))
				return 1;
		}
		
		if (svg_output_file) {
			if (!checkAndExport(root_geom, 2, OPENSCAD_SVG, svg_output_file))
				return 1;
		}

		if (png_output_file) {
			std::ofstream fstream(png_output_file,std::ios::out|std::ios::binary);
			if (!fstream.is_open()) {
				PRINTB("Can't open file \"%s\" for export", png_output_file);
			}
			else {
				if (renderer==Render::CGAL) {
					export_png(root_geom.get(), camera, fstream);
				} else if (renderer==Render::THROWNTOGETHER) {
					export_png_with_throwntogether(tree, camera, fstream);
				} else {
					export_png_with_opencsg(tree, camera, fstream);
				}
				fstream.close();
			}
		}
#else
		PRINT("OpenSCAD has been compiled without CGAL support!\n");
		return 1;
#endif
	}
	delete root_node;
	return 0;
}

#ifdef OPENSCAD_QTGUI
#include <QtPlugin>
#if defined(__MINGW64__) || defined(__MINGW32__) || defined(_MSCVER)
#if QT_VERSION < 0x050000
Q_IMPORT_PLUGIN(qtaccessiblewidgets)
#endif // QT_VERSION
#endif // MINGW64/MINGW32/MSCVER
#include "MainWindow.h"
  #ifdef __APPLE__
  #include "EventFilter.h"
  #endif
#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>

Q_DECLARE_METATYPE(shared_ptr<const Geometry>);

// Only if "fileName" is not absolute, prepend the "absoluteBase".
static QString assemblePath(const fs::path& absoluteBaseDir,
                            const string& fileName) {
  if (fileName.empty()) return "";
  QString qsDir = QString::fromLocal8Bit( boosty::stringy( absoluteBaseDir ).c_str() );
  QString qsFile = QString::fromLocal8Bit( fileName.c_str() );
  // if qsfile is absolute, dir is ignored. (see documentation of QFileInfo)
  QFileInfo info( qsDir, qsFile );
  return info.absoluteFilePath();
}

bool QtUseGUI()
{
#ifdef Q_OS_X11
	// see <http://qt.nokia.com/doc/4.5/qapplication.html#QApplication-2>:
	// On X11, the window system is initialized if GUIenabled is true. If GUIenabled
	// is false, the application does not connect to the X server. On Windows and
	// Macintosh, currently the window system is always initialized, regardless of the
	// value of GUIenabled. This may change in future versions of Qt.
	bool useGUI = getenv("DISPLAY") != 0;
#else
	bool useGUI = true;
#endif
	return useGUI;
}

int gui(vector<string> &inputFiles, const fs::path &original_path, int argc, char ** argv)
{
#ifdef Q_OS_MACX
    if (QSysInfo::MacintoshVersion > QSysInfo::MV_10_8) {
			// fix Mac OS X 10.9 (mavericks) font issue
			// https://bugreports.qt-project.org/browse/QTBUG-32789
			QFont::insertSubstitution(".Lucida Grande UI", "Lucida Grande");
    }
#endif
	QApplication app(argc, argv, true); //useGUI);
#ifdef Q_OS_MAC
	app.installEventFilter(new EventFilter(&app));
#endif
	// set up groups for QSettings
	QCoreApplication::setOrganizationName("OpenSCAD");
	QCoreApplication::setOrganizationDomain("openscad.org");
	QCoreApplication::setApplicationName("OpenSCAD");
	QCoreApplication::setApplicationVersion(TOSTRING(OPENSCAD_VERSION));
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
	QGuiApplication::setApplicationDisplayName("OpenSCAD");
#endif
	
	// Other global settings
	qRegisterMetaType<shared_ptr<const Geometry> >();
	
	const QString &app_path = app.applicationDirPath();

	QDir exdir(app_path);
	QString qexamplesdir;
#ifdef Q_OS_MAC
	exdir.cd("../Resources"); // Examples can be bundled
	if (!exdir.exists("examples")) exdir.cd("../../..");
#elif defined(Q_OS_UNIX)
	if (exdir.cd("../share/openscad/examples")) {
		qexamplesdir = exdir.path();
	} else
		if (exdir.cd("../../share/openscad/examples")) {
			qexamplesdir = exdir.path();
		} else
			if (exdir.cd("../../examples")) {
				qexamplesdir = exdir.path();
			} else
#endif
				if (exdir.cd("examples")) {
					qexamplesdir = exdir.path();
				}
	MainWindow::setExamplesDir(qexamplesdir);
  parser_init(app_path.toLocal8Bit().constData());

#ifdef Q_OS_MAC
	installAppleEventHandlers();
#endif

#if defined(OPENSCAD_DEPLOY) && defined(Q_OS_MAC)
	AutoUpdater *updater = new SparkleAutoUpdater;
	AutoUpdater::setUpdater(updater);
	if (updater->automaticallyChecksForUpdates()) updater->checkForUpdates();
#endif

#if 0 /*** disabled by clifford wolf: adds rendering artefacts with OpenCSG ***/
	// turn on anti-aliasing
	QGLFormat f;
	f.setSampleBuffers(true);
	f.setSamples(4);
	QGLFormat::setDefaultFormat(f);
#endif
	bool set = false;
	if (!inputFiles.size()) inputFiles.push_back(""); set = true;
#ifdef ENABLE_MDI
	BOOST_FOREACH(const string &infile, inputFiles) {
        MainWindow *mainwin = new MainWindow(assemblePath(original_path, infile));
	}
#else
	MainWindow *mainwin = new MainWindow(assemblePath(original_path, inputFiles[0]));
#endif
	if(set == true)
	mainwin->launcher->show();
	app.connect(&app, SIGNAL(lastWindowClosed()), &app, SLOT(quit()));
	int rc = app.exec();
	if (MainWindow::windows) {
		foreach (MainWindow *mainw, *MainWindow::windows) {
			delete mainw;
		}
	}
	return rc;
}
#else // OPENSCAD_QTGUI
bool QtUseGUI() { return false; }
int gui(const vector<string> &inputFiles, const fs::path &original_path, int argc, char ** argv)
{
	PRINT("Error: compiled without QT, but trying to run GUI\n");
	return 1;
}
#endif // OPENSCAD_QTGUI

int main(int argc, char **argv)
{
	int rc = 0;
	bool isGuiLaunched = getenv("GUI_LAUNCHED") != 0;
#ifdef Q_OS_MAC
	if (isGuiLaunched) set_output_handler(CocoaUtils::nslog, NULL);
#else
	PlatformUtils::ensureStdIO();
#endif
#ifdef ENABLE_CGAL
	// Causes CGAL errors to abort directly instead of throwing exceptions
	// (which we don't catch). This gives us stack traces without rerunning in gdb.
	CGAL::set_error_behaviour(CGAL::ABORT);
#endif
	Builtins::instance()->initialize();

	fs::path original_path = fs::current_path();

	const char *output_file = NULL;
	const char *deps_output_file = NULL;

	po::options_description desc("Allowed options");
	desc.add_options()
		("help,h", "help message")
		("version,v", "print the version")
		("info", "print information about the building process")
		("render", "if exporting a png image, do a full CGAL render")
		("preview", po::value<string>(), "if exporting a png image, do an OpenCSG(default) or ThrownTogether preview")
		("csglimit", po::value<unsigned int>(), "if exporting a png image, stop rendering at the given number of CSG elements")
		("camera", po::value<string>(), "parameters for camera when exporting png")
		("autocenter", "adjust camera to look at object center")
		("viewall", "adjust camera to fit object")
		("imgsize", po::value<string>(), "=width,height for exporting png")
		("projection", po::value<string>(), "(o)rtho or (p)erspective when exporting png")
		("debug", po::value<string>(), "special debug info")
		("o,o", po::value<string>(), "out-file")
		("s,s", po::value<string>(), "stl-file")
		("x,x", po::value<string>(), "dxf-file")
		("d,d", po::value<string>(), "deps-file")
		("m,m", po::value<string>(), "makefile")
		("D,D", po::value<vector<string> >(), "var=val")
#ifdef ENABLE_EXPERIMENTAL
		("enable", po::value<vector<string> >(), "enable experimental features")
#endif
		;

	po::options_description hidden("Hidden options");
	hidden.add_options()
		("input-file", po::value< vector<string> >(), "input file");

	po::positional_options_description p;
	p.add("input-file", -1);

	po::options_description all_options;
	all_options.add(desc).add(hidden);

	po::variables_map vm;
	try {
		po::store(po::command_line_parser(argc, argv).options(all_options).allow_unregistered().positional(p).run(), vm);
	}
	catch(const std::exception &e) { // Catches e.g. unknown options
		PRINTB("%s\n", e.what());
		help(argv[0]);
	}

	OpenSCAD::debug = "";
	if (vm.count("debug")) {
		OpenSCAD::debug = vm["debug"].as<string>();
		PRINTB("Debug on. --debug=%s",OpenSCAD::debug);
	}
	if (vm.count("help")) help(argv[0]);
	if (vm.count("version")) version();
	if (vm.count("info")) info();

	Render::type renderer = Render::OPENCSG;
	if (vm.count("render"))
		renderer = Render::CGAL;
	if (vm.count("preview"))
		if (vm["preview"].as<string>() == "throwntogether")
			renderer = Render::THROWNTOGETHER;

	if (vm.count("csglimit")) {
		RenderSettings::inst()->openCSGTermLimit = vm["csglimit"].as<unsigned int>();
	}

	if (vm.count("o")) {
		// FIXME: Allow for multiple output files?
		if (output_file) help(argv[0]);
		output_file = vm["o"].as<string>().c_str();
	}
	if (vm.count("s")) {
		printDeprecation("DEPRECATED: The -s option is deprecated. Use -o instead.\n");
		if (output_file) help(argv[0]);
		output_file = vm["s"].as<string>().c_str();
	}
	if (vm.count("x")) { 
		printDeprecation("DEPRECATED: The -x option is deprecated. Use -o instead.\n");
		if (output_file) help(argv[0]);
		output_file = vm["x"].as<string>().c_str();
	}
	if (vm.count("d")) {
		if (deps_output_file)
			help(argv[0]);
		deps_output_file = vm["d"].as<string>().c_str();
	}
	if (vm.count("m")) {
		if (make_command)
			help(argv[0]);
		make_command = vm["m"].as<string>().c_str();
	}

	if (vm.count("D")) {
		BOOST_FOREACH(const string &cmd, vm["D"].as<vector<string> >()) {
			commandline_commands += cmd;
			commandline_commands += ";\n";
		}
	}
#ifdef ENABLE_EXPERIMENTAL
	if (vm.count("enable")) {
		BOOST_FOREACH(const string &feature, vm["enable"].as<vector<string> >()) {
			Feature::enable_feature(feature);
		}
	}
#endif
	vector<string> inputFiles;
	if (vm.count("input-file"))	{
		inputFiles = vm["input-file"].as<vector<string> >();
	}
#ifndef ENABLE_MDI
	if (inputFiles.size() > 1) {
		help(argv[0]);
	}
#endif

	currentdir = boosty::stringy(fs::current_path());

	Camera camera = get_camera(vm);

	// Initialize global visitors
	NodeCache nodecache;
	NodeDumper dumper(nodecache);

	bool cmdlinemode = false;
	if (output_file) { // cmd-line mode
		cmdlinemode = true;
		if (!inputFiles.size()) help(argv[0]);
	}

	if (cmdlinemode) {
		rc = cmdline(deps_output_file, inputFiles[0], camera, output_file, original_path, renderer, argc, argv);
	}
	else if (QtUseGUI()) {
		rc = gui(inputFiles, original_path, argc, argv);
	}
	else {
		PRINT("Requested GUI mode but can't open display!\n");
		help(argv[0]);
	}

	Builtins::instance(true);

	return rc;
}

